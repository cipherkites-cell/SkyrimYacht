// SkyrimYacht - a dice game for Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Overlay.h"

#include "Config.h"
#include "Game.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_impl_dx11.h>

namespace Overlay
{
	namespace
	{
		struct InputMsg
		{
			enum class Kind
			{
				MouseMove,
				MouseButton,
				Wheel,
				Key
			} kind;
			float x = 0.0f, y = 0.0f;
			int   button = 0;
			bool  down = false;
		};
		std::mutex            g_inputMtx;
		std::vector<InputMsg> g_input;   // for ImGui (drained in Present)
		std::vector<Press>    g_presses;  // for the table
		std::vector<int>      g_keys;

		std::atomic<bool>  g_open{ false };
		std::atomic<bool>  g_inputDisabled{ false };
		std::atomic<bool>  g_uiHover{ false };
		std::atomic<bool>  g_held[3]{};
		std::atomic<float> g_mx{ 0.0f }, g_my{ 0.0f };
		std::atomic<float> g_screenW{ 1920.0f }, g_screenH{ 1080.0f };
		std::atomic<bool>  g_center{ true };
		HWND               g_hwnd = nullptr;

		bool                 g_imguiReady = false;
		bool                 g_imguiFailed = false;
		IDXGISwapChain*      g_sc = nullptr;
		ID3D11Device*        g_dev = nullptr;
		ID3D11DeviceContext* g_ctx = nullptr;
		std::chrono::steady_clock::time_point g_lastFrame{};

		void Push(const InputMsg& a_m)
		{
			std::lock_guard lk(g_inputMtx);
			if (g_input.size() < 1024) {
				g_input.push_back(a_m);
			}
		}

		void HandleInput(RE::InputEvent* const* a_event)
		{
			if (!a_event || !g_open) {
				return;
			}
			for (auto* e = *a_event; e; e = e->next) {
				if (e->GetEventType() == RE::INPUT_EVENT_TYPE::kMouseMove) {
					auto*       mm = static_cast<RE::MouseMoveEvent*>(e);
					const float s = Config::Get().mouseSpeed;
					// the pointer itself lives here, so the table and ImGui agree on where it is
					g_mx = std::clamp(g_mx.load() + mm->mouseInputX * s, 0.0f, g_screenW.load() - 1.0f);
					g_my = std::clamp(g_my.load() + mm->mouseInputY * s, 0.0f, g_screenH.load() - 1.0f);
					Push({ InputMsg::Kind::MouseMove, g_mx.load(), g_my.load() });
					continue;
				}
				auto* btn = e->AsButtonEvent();
				if (!btn) {
					continue;
				}
				const auto code = btn->GetIDCode();
				const bool down = btn->IsDown();
				const bool up = btn->IsUp();
				if (!down && !up) {
					continue;
				}
				if (e->GetDevice() == RE::INPUT_DEVICE::kMouse) {
					using MouseKey = RE::BSWin32MouseDevice::Key;
					if (code == MouseKey::kWheelUp || code == MouseKey::kWheelDown) {
						if (down) {
							Push({ InputMsg::Kind::Wheel, 0.0f, code == MouseKey::kWheelUp ? 1.0f : -1.0f });
						}
					} else if (code <= 2) {
						g_held[code] = down;
						Push({ InputMsg::Kind::MouseButton, g_mx.load(), g_my.load(), static_cast<int>(code), down });
						std::lock_guard lk(g_inputMtx);
						g_presses.push_back({ g_mx.load(), g_my.load(), static_cast<int>(code), down, down && g_uiHover.load() });
					}
				} else if (e->GetDevice() == RE::INPUT_DEVICE::kKeyboard && down) {
					std::lock_guard lk(g_inputMtx);
					g_keys.push_back(static_cast<int>(code));
				}
				if (Config::Get().logInput) {
					spdlog::info("[input] device {} code {:X} {}", static_cast<int>(e->GetDevice()), code, down ? "down" : "up");
				}
			}
		}

		void SafeHandleInput(RE::InputEvent* const* a_event)
		{
			__try {
				HandleInput(a_event);
			} __except (SehFilter(GetExceptionInformation(), "HandleInput")) {
				g_inputDisabled = true;
			}
		}

		// Hook on the call that hands each frame's input to the game and to every other mod: while a game is open
		// nothing gets through, so the mouse can't turn the camera and keys can't open other menus.
		using DispatchFn = void (*)(RE::BSTEventSource<RE::InputEvent*>*, RE::InputEvent* const*);
		DispatchFn g_origDispatch = nullptr;

		void DispatchHook(RE::BSTEventSource<RE::InputEvent*>* a_dispatcher, RE::InputEvent* const* a_events)
		{
			if (!g_inputDisabled) {
				SafeHandleInput(a_events);
			}
			if (g_open) {
				static RE::InputEvent* s_none[1] = { nullptr };
				s_none[0] = nullptr;
				g_origDispatch(a_dispatcher, s_none);
				return;
			}
			g_origDispatch(a_dispatcher, a_events);
		}

		bool InstallDispatchHook()
		{
			// BSInputDeviceManager: the SendEvent call inside the input polling function (SE 67315, AE 68617)
			REL::Relocation<std::uintptr_t> target{ RELOCATION_ID(67315, 68617), 0x7B };
			const auto*                     op = reinterpret_cast<const std::uint8_t*>(target.address());
			if (*op != 0xE8) {
				spdlog::warn("Input hook: expected a call at {:X}, found {:02X}", target.address(), *op);
				return false;
			}
			g_origDispatch = reinterpret_cast<DispatchFn>(SKSE::GetTrampoline().write_call<5>(target.address(), &DispatchHook));
			return g_origDispatch != nullptr;
		}

		bool SafeInstallDispatchHook()
		{
			__try {
				return InstallDispatchHook();
			} __except (SehFilter(GetExceptionInformation(), "InstallDispatchHook")) {
				return false;
			}
		}

		class InputSink final : public RE::BSTEventSink<RE::InputEvent*>
		{
		public:
			RE::BSEventNotifyControl ProcessEvent(RE::InputEvent* const* a_event, RE::BSTEventSource<RE::InputEvent*>*) override
			{
				if (!g_inputDisabled) {
					SafeHandleInput(a_event);
				}
				return RE::BSEventNotifyControl::kContinue;
			}
		};

		void DrainInput()
		{
			std::vector<InputMsg> msgs;
			{
				std::lock_guard lk(g_inputMtx);
				msgs.swap(g_input);
			}
			auto& io = ImGui::GetIO();
			for (const auto& m : msgs) {
				switch (m.kind) {
				case InputMsg::Kind::MouseMove:
					io.AddMousePosEvent(m.x, m.y);
					break;
				case InputMsg::Kind::MouseButton:
					io.AddMousePosEvent(m.x, m.y);
					io.AddMouseButtonEvent(m.button, m.down);
					break;
				case InputMsg::Kind::Wheel:
					io.AddMouseWheelEvent(0.0f, m.y);
					break;
				default:
					break;
				}
			}
			// a release the game never saw (alt-tab, the Steam overlay): let go
			static constexpr int kVk[3] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON };
			for (int b = 0; b < 3; ++b) {
				if (io.MouseDown[b] && (::GetAsyncKeyState(kVk[b]) & 0x8000) == 0) {
					io.AddMouseButtonEvent(b, false);
					g_held[b] = false;
				}
			}
		}

		bool InitImGui()
		{
			IMGUI_CHECKVERSION();
			ImGui::CreateContext();
			auto& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			io.LogFilename = nullptr;
			io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
			Game::SetupUi(g_screenH.load());
			return ImGui_ImplDX11_Init(g_dev, g_ctx);
		}
	}

	void SetOpen(bool a_open)
	{
		if (g_open == a_open) {
			return;
		}
		g_open = a_open;
		for (auto& h : g_held) {
			h = false;
		}
		{
			std::lock_guard lk(g_inputMtx);
			g_input.clear();
			g_presses.clear();
			g_keys.clear();
		}
		if (a_open) {
			g_center = true;
		}
	}

	bool IsOpen() { return g_open.load(); }

	void MousePos(float& a_x, float& a_y)
	{
		a_x = g_mx.load();
		a_y = g_my.load();
	}

	std::vector<Press> TakePresses()
	{
		std::lock_guard    lk(g_inputMtx);
		std::vector<Press> out;
		out.swap(g_presses);
		return out;
	}

	std::vector<int> TakeKeys()
	{
		std::lock_guard  lk(g_inputMtx);
		std::vector<int> out;
		out.swap(g_keys);
		return out;
	}

	bool ButtonHeld(int a_button) { return a_button >= 0 && a_button < 3 && g_held[a_button].load(); }

	std::string ToUtf8(const std::wstring& a_w)
	{
		if (a_w.empty()) {
			return {};
		}
		const int   n = ::WideCharToMultiByte(CP_UTF8, 0, a_w.c_str(), static_cast<int>(a_w.size()), nullptr, 0, nullptr, nullptr);
		std::string s(n, '\0');
		::WideCharToMultiByte(CP_UTF8, 0, a_w.c_str(), static_cast<int>(a_w.size()), s.data(), n, nullptr, nullptr);
		return s;
	}

	int SehFilter(EXCEPTION_POINTERS* a_info, const char* a_where)
	{
		const auto* rec = a_info ? a_info->ExceptionRecord : nullptr;
		const auto  addr = rec ? reinterpret_cast<std::uintptr_t>(rec->ExceptionAddress) : 0;
		HMODULE     mod = nullptr;
		wchar_t     modPath[MAX_PATH]{};
		if (addr && ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
						reinterpret_cast<LPCWSTR>(addr), &mod)) {
			::GetModuleFileNameW(mod, modPath, MAX_PATH);
		}
		const auto offset = mod ? addr - reinterpret_cast<std::uintptr_t>(mod) : addr;
		spdlog::critical("CRASH CAUGHT in {}: exception 0x{:08X} at {}+0x{:X} - this part is now switched off so the game keeps running",
			a_where, rec ? static_cast<std::uint32_t>(rec->ExceptionCode) : 0u, ToUtf8(std::filesystem::path(modPath).filename().wstring()), offset);
		return EXCEPTION_EXECUTE_HANDLER;
	}

	bool InstallInputSink()
	{
		if (SafeInstallDispatchHook()) {
			spdlog::info("Input hook installed: the game and other mods get no input while a game is open");
			return true;
		}
		static InputSink sink;
		auto*            mgr = RE::BSInputDeviceManager::GetSingleton();
		if (!mgr) {
			return false;
		}
		mgr->AddEventSink<RE::InputEvent*>(&sink);
		return true;
	}

	void OnPresent(IDXGISwapChain* sc)
	{
		if (!g_sc) {
			DXGI_SWAP_CHAIN_DESC desc{};
			if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow) {
				return;
			}
			if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), reinterpret_cast<void**>(&g_dev))) || !g_dev) {
				return;
			}
			g_dev->GetImmediateContext(&g_ctx);
			g_sc = sc;
			g_hwnd = desc.OutputWindow;
			g_screenW = static_cast<float>(desc.BufferDesc.Width);
			g_screenH = static_cast<float>(desc.BufferDesc.Height);
			spdlog::info("Attached to the game's swap chain ({}x{})", desc.BufferDesc.Width, desc.BufferDesc.Height);
		}
		if (sc != g_sc) {
			return;
		}
		if (!g_open) {
			return;
		}
		if (!g_imguiReady && !g_imguiFailed) {
			if (!InitImGui() || !ImGui_ImplDX11_CreateDeviceObjects()) {
				g_imguiFailed = true;
				spdlog::error("ImGui failed to initialise");
				return;
			}
			g_imguiReady = true;
		}
		if (g_imguiFailed) {
			return;
		}

		ID3D11Texture2D* back = nullptr;
		if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&back))) || !back) {
			return;
		}
		D3D11_TEXTURE2D_DESC bd{};
		back->GetDesc(&bd);
		g_screenW = static_cast<float>(bd.Width);
		g_screenH = static_cast<float>(bd.Height);

		auto& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(static_cast<float>(bd.Width), static_cast<float>(bd.Height));
		const auto  now = std::chrono::steady_clock::now();
		const float dt = g_lastFrame.time_since_epoch().count() ? std::chrono::duration<float>(now - g_lastFrame).count() : 1.0f / 60.0f;
		g_lastFrame = now;
		io.DeltaTime = std::clamp(dt, 1.0f / 1000.0f, 0.5f);
		if (g_center.exchange(false)) {
			g_mx = io.DisplaySize.x * 0.5f;
			g_my = io.DisplaySize.y * 0.6f;
			io.AddMousePosEvent(g_mx.load(), g_my.load());
		}
		DrainInput();
		io.MouseDrawCursor = true;

		ImGui_ImplDX11_NewFrame();
		ImGui::NewFrame();
		Game::DrawUi();
		g_uiHover = io.WantCaptureMouse;
		ImGui::Render();

		ID3D11RenderTargetView* rtv = nullptr;
		g_dev->CreateRenderTargetView(back, nullptr, &rtv);
		back->Release();
		if (rtv) {
			g_ctx->OMSetRenderTargets(1, &rtv, nullptr);
			ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
			rtv->Release();
		}
	}
}
