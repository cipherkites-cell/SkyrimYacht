// SkyrimYacht - a dice game for Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Scene.h"

#include "Config.h"
#include "EspIds.h"
#include "Game.h"
#include "Overlay.h"

// The inventory 3D calls CommonLibSSE-NG doesn't wrap by these names. IDs as used by Grid Inventory (GPL-3.0) and
// Mod Explorer Menu, which show the game's item preview in their own menus the same way.
namespace Inv3D
{
	void Begin3D(RE::Inventory3DManager* a_mgr, RE::INTERFACE_LIGHT_SCHEME a_scheme)
	{
		using func_t = void (*)(RE::Inventory3DManager*, RE::INTERFACE_LIGHT_SCHEME);
		static REL::Relocation<func_t> func{ RELOCATION_ID(50881, 51754) };
		func(a_mgr, a_scheme);
	}
	void End3D(RE::Inventory3DManager* a_mgr)
	{
		using func_t = void (*)(RE::Inventory3DManager*);
		static REL::Relocation<func_t> func{ RELOCATION_ID(50883, 51756) };
		func(a_mgr);
	}
	void Load(RE::Inventory3DManager* a_mgr, RE::TESBoundObject* a_obj, RE::ExtraDataList* a_extra)
	{
		using func_t = void (*)(RE::Inventory3DManager*, RE::TESBoundObject*, RE::ExtraDataList*);
		static REL::Relocation<func_t> func{ RELOCATION_ID(50885, 51758) };
		func(a_mgr, a_obj, a_extra);
	}
}

namespace Scene
{
	namespace
	{
		constexpr std::string_view kMenuName = "SkyrimYachtMenu";
		constexpr const char*      kDiceSets[] = { "bone", "iron", "steel", "corundum", "dwemer", "orichalcum", "silver", "quicksilver",
				 "moonstone", "gold", "malachite", "ebony", "stalhrim", "dragonbone", "daedric" };

		// The cups: the vanilla meshes, scaled so their inside radius matches the physics' cup (radius 1.9) with a
		// little room, and lowered so their inside floor sits at the cup's origin (tools/make_local_cups.py measured them).
		struct CupModel
		{
			const char* id;
			const char* file;  // the vanilla mesh's file name; its path comes from the game's own item
			float       floor, inner, room;
		};
		constexpr CupModel kCups[] = {
			{ "tankard", "basictankard01.nif", 2.9f, 4.32f, 1.04f },
			{ "goblet", "glazedgoblet01.nif", 6.6f, 4.6f, 1.1f },
			{ "dwemer", "dwecup01.nif", 0.8f, 4.36f, 1.22f },
			{ "silver", "silvergoblet01.nif", 7.8f, 5.2f, 1.1f },
			{ "golden", "tggoldengoblet01.nif", 8.3f, 4.19f, 1.1f },
		};
		constexpr float kCupRadius = 1.9f;
		constexpr float kTableHalfWidth = 19.6f;  // tray, and the cups resting beside it

		bool                     g_open = false;
		bool                     g_shown = false;   // our menu is on screen
		bool                     g_scene = false;   // Begin3D called, End3D not yet
		bool                     g_teardown = false;
		bool                     g_attached = false;
		RE::TESBoundObject*      g_tray = nullptr;
		RE::NiPointer<RE::NiNode> g_root;           // ours, under the tray
		RE::NiPointer<RE::NiAVObject> g_dice[2][5];  // [0] the player's set, [1] bone (the opponent's)
		RE::NiPointer<RE::NiAVObject> g_cupNode[2];
		std::string              g_diceSet = "bone", g_loadedSet;
		std::string              g_cupId[2] = { "tankard", "tankard" }, g_loadedCup[2];
		int                      g_diceSide = 0;    // which set is showing
		std::unordered_map<std::string, std::string> g_cupPaths;
		RE::NiAVObject*          g_model = nullptr;

		// The table's placement this frame (in the inventory scene's camera space: y away from the camera, z up).
		RE::NiMatrix3 g_rot;
		RE::NiPoint3  g_pos;
		float         g_scale = 1.0f;
		RE::NiFrustum g_frustum{};
		float         g_screenW = 1920.0f, g_screenH = 1080.0f;
		bool          g_placed = false;

		RE::NiMatrix3 FromQuat(const yacht::Quat& q)
		{
			RE::NiMatrix3 m;
			const double  w = q.w, x = q.x, y = q.y, z = q.z;
			m.entry[0][0] = static_cast<float>(1 - 2 * (y * y + z * z));
			m.entry[0][1] = static_cast<float>(2 * (x * y - w * z));
			m.entry[0][2] = static_cast<float>(2 * (x * z + w * y));
			m.entry[1][0] = static_cast<float>(2 * (x * y + w * z));
			m.entry[1][1] = static_cast<float>(1 - 2 * (x * x + z * z));
			m.entry[1][2] = static_cast<float>(2 * (y * z - w * x));
			m.entry[2][0] = static_cast<float>(2 * (x * z - w * y));
			m.entry[2][1] = static_cast<float>(2 * (y * z + w * x));
			m.entry[2][2] = static_cast<float>(1 - 2 * (x * x + y * y));
			return m;
		}

		RE::NiPoint3 Mul(const RE::NiMatrix3& m, const RE::NiPoint3& p)
		{
			return { m.entry[0][0] * p.x + m.entry[0][1] * p.y + m.entry[0][2] * p.z, m.entry[1][0] * p.x + m.entry[1][1] * p.y + m.entry[1][2] * p.z,
				m.entry[2][0] * p.x + m.entry[2][1] * p.y + m.entry[2][2] * p.z };
		}

		RE::NiPointer<RE::NiNode> Demand(const std::string& a_path)
		{
			RE::NiPointer<RE::NiNode>             node;
			RE::BSModelDB::DBTraits::ArgsType     args{};
			const auto                            err = RE::BSModelDB::Demand(a_path.c_str(), node, args);
			if (!node) {
				spdlog::warn("Could not load mesh {} (error {})", a_path, static_cast<int>(err));
			}
			return node;
		}

		RE::NiPointer<RE::NiAVObject> Instance(const std::string& a_path)
		{
			auto base = Demand(a_path);
			if (!base) {
				return nullptr;
			}
			// each die and cup needs its own copy: the database hands out one shared node per file
			return RE::NiPointer<RE::NiAVObject>(base->Clone());
		}

		void FindCupPaths()
		{
			if (!g_cupPaths.empty()) {
				return;
			}
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return;
			}
			for (auto* misc : dh->GetFormArray<RE::TESObjectMISC>()) {
				const char* model = misc ? misc->GetModel() : nullptr;
				if (!model || !*model) {
					continue;
				}
				std::string m = model;
				std::string low = m;
				std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				for (const auto& c : kCups) {
					if (low.size() >= std::strlen(c.file) && low.compare(low.size() - std::strlen(c.file), std::strlen(c.file), c.file) == 0 &&
						!g_cupPaths.contains(c.id)) {
						g_cupPaths[c.id] = m;
						spdlog::info("Cup {}: {}", c.id, m);
					}
				}
			}
		}

		const CupModel& Cup(const std::string& a_id)
		{
			for (const auto& c : kCups) {
				if (a_id == c.id) {
					return c;
				}
			}
			return kCups[0];
		}

		void Detach(RE::NiPointer<RE::NiAVObject>& a_obj)
		{
			if (a_obj && g_root) {
				g_root->DetachChild2(a_obj.get());
			}
			a_obj.reset();
		}

		void LoadDice()
		{
			if (!g_root || g_loadedSet == g_diceSet) {
				return;
			}
			for (int i = 0; i < 5; ++i) {
				Detach(g_dice[0][i]);
			}
			for (int i = 0; i < 5; ++i) {
				g_dice[0][i] = Instance("SkyrimYacht\\Die_" + g_diceSet + ".nif");
				if (g_dice[0][i]) {
					g_root->AttachChild(g_dice[0][i].get(), true);
				}
			}
			g_loadedSet = g_diceSet;
			spdlog::info("Dice set {}", g_diceSet);
		}

		void LoadCup(int a_which)
		{
			if (!g_root || g_loadedCup[a_which] == g_cupId[a_which]) {
				return;
			}
			Detach(g_cupNode[a_which]);
			FindCupPaths();
			const auto& c = Cup(g_cupId[a_which]);
			const auto  it = g_cupPaths.find(c.id);
			if (it == g_cupPaths.end()) {
				spdlog::warn("No item in the game uses {}: the {} won't show", c.file, c.id);
				g_loadedCup[a_which] = g_cupId[a_which];
				return;
			}
			// the game's own paths are relative to Data\Meshes already
			auto mesh = Instance(it->second);
			if (!mesh) {
				g_loadedCup[a_which] = g_cupId[a_which];
				return;
			}
			// a holder: the cup's origin is the physics cup's (the centre of its inside floor)
			auto holder = RE::NiPointer<RE::NiNode>(RE::NiNode::Create(1));
			const float s = kCupRadius / c.inner * c.room;
			mesh->local.scale = s;
			mesh->local.translate = { 0.0f, 0.0f, -c.floor * s };
			holder->AttachChild(mesh.get(), true);
			g_cupNode[a_which] = RE::NiPointer<RE::NiAVObject>(holder.get());
			g_root->AttachChild(holder.get(), true);
			g_loadedCup[a_which] = g_cupId[a_which];
		}

		bool LoadInFlight(RE::Inventory3DManager* a_mgr)
		{
			auto& rt = a_mgr->GetRuntimeData();
			if (rt.loadTask) {
				return true;
			}
			for (auto& lm : rt.loadedModels) {
				if (!lm.spModel || lm.spModel->worldBound.radius <= 0.0f) {
					return true;
				}
			}
			return false;
		}

		RE::NiAVObject* TrayModel(RE::Inventory3DManager* a_mgr)
		{
			auto& rt = a_mgr->GetRuntimeData();
			if (rt.loadTask || rt.loadedModels.empty()) {
				return nullptr;
			}
			for (auto& lm : rt.loadedModels) {
				if ((lm.itemBase == g_tray || lm.modelObj == g_tray) && lm.spModel) {
					return lm.spModel.get();
				}
			}
			return rt.loadedModels.back().spModel.get();
		}

		bool TryTeardown()
		{
			auto* mgr = RE::Inventory3DManager::GetSingleton();
			if (!mgr || !g_scene) {
				return true;
			}
			if (LoadInFlight(mgr)) {
				return false;  // End3D walks the loaded models: not while one is half made
			}
			if (g_root && g_model) {
				if (auto* node = g_model->AsNode()) {
					node->DetachChild2(g_root.get());
				}
			}
			Inv3D::End3D(mgr);
			g_scene = false;
			g_model = nullptr;
			spdlog::info("Table put away");
			return true;
		}

		// ---- the menu: pauses the game, blurs the world, and is where the item preview gets drawn
		class YachtMenu : public RE::IMenu
		{
		public:
			static RE::IMenu* Create()
			{
				using Flag = RE::UI_MENU_FLAGS;
				auto* m = new YachtMenu();
				m->menuFlags.set(Flag::kPausesGame, Flag::kDisablePauseMenu, Flag::kUsesMenuContext, Flag::kCustomRendering,
					Flag::kRequiresUpdate);
				m->depthPriority = 10;
				return m;
			}

			RE::UI_MESSAGE_RESULTS ProcessMessage(RE::UIMessage& a_message) override
			{
				switch (a_message.type.get()) {
				case RE::UI_MESSAGE_TYPE::kShow:
					OnShow();
					break;
				case RE::UI_MESSAGE_TYPE::kHide:
				case RE::UI_MESSAGE_TYPE::kForceHide:
					OnHide();
					break;
				default:
					break;
				}
				return RE::IMenu::ProcessMessage(a_message);
			}

			void AdvanceMovie(float a_interval, std::uint32_t a_currentTime) override
			{
				// menus keep advancing while the game is paused: the table and the game run here, before the frame draws
				__try {
					Game::Tick(a_interval);
					Scene::Update();
				} __except (Overlay::SehFilter(GetExceptionInformation(), "Tick")) {
					g_failed = true;
				}
				if (g_failed) {
					Game::Abort();
				}
				RE::IMenu::AdvanceMovie(a_interval, a_currentTime);
			}

			void PostDisplay() override
			{
				// the game's UI pass: the only place the item preview actually draws
				if (auto* mgr = RE::Inventory3DManager::GetSingleton(); mgr && g_scene) {
					mgr->Render();
				}
			}

		private:
			static void OnShow()
			{
				g_shown = true;
				if (auto* blur = RE::UIBlurManager::GetSingleton()) {
					blur->IncrementBlurCount();
				}
				auto* mgr = RE::Inventory3DManager::GetSingleton();
				if (!mgr) {
					spdlog::error("Inventory3DManager missing: the table can't be shown");
					return;
				}
				if (g_scene && !TryTeardown()) {
					spdlog::warn("The last table was still loading; opening a fresh one anyway");
					g_scene = false;
				}
				Inv3D::Begin3D(mgr, RE::INTERFACE_LIGHT_SCHEME::kInventory);
				g_scene = true;
				g_attached = false;
				g_placed = false;
				g_model = nullptr;
				Inv3D::Load(mgr, g_tray, nullptr);
				spdlog::info("Table: loading the tray");
			}

			static void OnHide()
			{
				if (!g_shown) {
					return;
				}
				g_shown = false;
				if (auto* blur = RE::UIBlurManager::GetSingleton()) {
					blur->DecrementBlurCount();
				}
				g_teardown = !TryTeardown();
			}

			static inline bool g_failed = false;
		};
	}

	void Register()
	{
		g_tray = RE::TESDataHandler::GetSingleton()->LookupForm<RE::TESObjectMISC>(EspIds::kTray, EspIds::kPlugin);
		if (!g_tray) {
			spdlog::error("YA_Tray not found in {} - is the plugin enabled?", EspIds::kPlugin);
		}
		if (auto* ui = RE::UI::GetSingleton()) {
			ui->Register(kMenuName, YachtMenu::Create);
			spdlog::info("Menu registered");
		}
		g_rot = RE::NiMatrix3();
	}

	void Open()
	{
		if (g_open || !g_tray) {
			return;
		}
		g_open = true;
		Overlay::SetOpen(true);
		if (auto* q = RE::UIMessageQueue::GetSingleton()) {
			q->AddMessage(kMenuName, RE::UI_MESSAGE_TYPE::kShow, nullptr);
		}
	}

	void Close()
	{
		if (!g_open) {
			return;
		}
		g_open = false;
		Overlay::SetOpen(false);
		if (auto* q = RE::UIMessageQueue::GetSingleton()) {
			q->AddMessage(kMenuName, RE::UI_MESSAGE_TYPE::kHide, nullptr);
		}
	}

	bool IsOpen() { return g_open; }
	bool Ready() { return g_attached && g_placed; }

	void UseDice(const std::string& a_set)
	{
		g_diceSet = "bone";
		for (const char* s : kDiceSets) {
			if (a_set == s) {
				g_diceSet = a_set;
			}
		}
	}

	void UseCup(int a_which, const std::string& a_cupId) { g_cupId[std::clamp(a_which, 0, 1)] = a_cupId; }

	void SetDie(int a_index, const yacht::Vec3& a_pos, const yacht::Quat& a_rot, bool a_visible)
	{
		if (a_index < 0 || a_index >= 5) {
			return;
		}
		auto& d = g_dice[0][a_index];
		if (!d) {
			return;
		}
		d->local.translate = { static_cast<float>(a_pos.x), static_cast<float>(a_pos.y), static_cast<float>(a_pos.z) };
		d->local.rotate = FromQuat(a_rot);
		d->CullNode(!a_visible);
	}

	void SetCup(int a_which, const yacht::Vec3& a_pos, const yacht::Quat& a_rot)
	{
		auto& c = g_cupNode[std::clamp(a_which, 0, 1)];
		if (!c) {
			return;
		}
		c->local.translate = { static_cast<float>(a_pos.x), static_cast<float>(a_pos.y), static_cast<float>(a_pos.z) };
		c->local.rotate = FromQuat(a_rot);
	}

	void Update()
	{
		auto* mgr = RE::Inventory3DManager::GetSingleton();
		if (!mgr || !g_scene) {
			return;
		}
		auto* model = TrayModel(mgr);
		if (!model || model->worldBound.radius <= 0.0f) {
			return;
		}
		if (model != g_model) {
			g_model = model;
			g_attached = false;
		}
		if (!g_attached) {
			auto* node = model->AsNode();
			if (!node) {
				spdlog::error("The tray loaded without a node to hang the dice on");
				return;
			}
			g_root = RE::NiPointer<RE::NiNode>(RE::NiNode::Create(8));
			node->AttachChild(g_root.get(), true);
			g_loadedSet.clear();
			g_loadedCup[0].clear();
			g_loadedCup[1].clear();
			for (auto& c : g_cupNode) {
				c.reset();
			}
			for (auto& d : g_dice[0]) {
				d.reset();
			}
			g_attached = true;
			spdlog::info("Table: tray loaded (bound radius {:.1f}), dice and cups attached", model->worldBound.radius);
		}
		LoadDice();
		LoadCup(0);
		LoadCup(1);

		// Place the table: tilted toward you, scaled to fit the screen, centred where the settings say.
		if (auto* scn = RE::UI3DSceneManager::GetSingleton()) {
			g_frustum = scn->viewFrustum;
		}
		if (auto* r = RE::BSGraphics::Renderer::GetSingleton()) {
			const auto& win = r->data.renderWindows[0];
			g_screenW = static_cast<float>(win.windowWidth > 0 ? win.windowWidth : 1920);
			g_screenH = static_cast<float>(win.windowHeight > 0 ? win.windowHeight : 1080);
		}
		const auto& cfg = Config::Get();
		static float s_depth = 0.0f;
		if (!g_placed) {
			s_depth = cfg.depth > 0.0f ? cfg.depth : (std::max)(model->local.translate.y, 10.0f);
			spdlog::info("Table: engine placed the tray at ({:.1f}, {:.1f}, {:.1f}) scale {:.3f}; frustum L {:.3f} R {:.3f} T {:.3f} B {:.3f}",
				model->local.translate.x, model->local.translate.y, model->local.translate.z, model->local.scale, g_frustum.fLeft,
				g_frustum.fRight, g_frustum.fTop, g_frustum.fBottom);
		}
		const float d = s_depth;
		const float spanX = std::fabs(g_frustum.fLeft - g_frustum.fRight) * d;
		g_scale = spanX > 0.0f ? cfg.fitWidth * spanX / (2.0f * kTableHalfWidth) : 1.0f;
		const float tilt = (cfg.flipTilt ? -1.0f : 1.0f) * cfg.tiltDeg * 0.0174533f;
		g_rot.entry[0][0] = 1.0f, g_rot.entry[0][1] = 0.0f, g_rot.entry[0][2] = 0.0f;
		g_rot.entry[1][0] = 0.0f, g_rot.entry[1][1] = std::cos(tilt), g_rot.entry[1][2] = -std::sin(tilt);
		g_rot.entry[2][0] = 0.0f, g_rot.entry[2][1] = std::sin(tilt), g_rot.entry[2][2] = std::cos(tilt);
		// the screen point -> camera space at depth d (the inverse of Project)
		const float rx = (g_frustum.fLeft - g_frustum.fRight) * d / g_screenW;
		const float rz = (g_frustum.fBottom - g_frustum.fTop) * d / g_screenH;
		g_pos = { -(cfg.centerX * g_screenW) * rx + g_frustum.fLeft * d, d, -(cfg.centerY * g_screenH) * rz + g_frustum.fBottom * d };
		model->local.rotate = g_rot;
		model->local.scale = g_scale;
		model->local.translate = g_pos;
		RE::NiUpdateData ud;
		model->Update(ud);
		if (!g_placed) {
			spdlog::info("Table: placed at ({:.1f}, {:.1f}, {:.1f}), scale {:.3f}, tilt {:.0f} degrees", g_pos.x, g_pos.y, g_pos.z, g_scale, cfg.tiltDeg);
		}
		g_placed = true;
	}

	bool Project(const yacht::Vec3& a_p, float& a_sx, float& a_sy)
	{
		if (!g_placed) {
			return false;
		}
		const RE::NiPoint3 local{ static_cast<float>(a_p.x) * g_scale, static_cast<float>(a_p.y) * g_scale, static_cast<float>(a_p.z) * g_scale };
		const RE::NiPoint3 w = Mul(g_rot, local) + g_pos;
		if (w.y <= 0.01f) {
			return false;
		}
		a_sx = (g_frustum.fLeft * w.y - w.x) / ((g_frustum.fLeft - g_frustum.fRight) * w.y) * g_screenW;
		a_sy = (g_frustum.fBottom * w.y - w.z) / ((g_frustum.fBottom - g_frustum.fTop) * w.y) * g_screenH;
		return std::isfinite(a_sx) && std::isfinite(a_sy);
	}

	bool Unproject(float a_sx, float a_sy, double a_planeZ, double& a_x, double& a_y)
	{
		// The plane z = a_planeZ maps onto the screen by a projective map: fit it from four points and invert it.
		const double px[4] = { -10, 10, 10, -10 }, py[4] = { -10, -10, 10, 10 };
		double       sx[4], sy[4];
		for (int k = 0; k < 4; ++k) {
			float fx = 0, fy = 0;
			if (!Project({ px[k], py[k], a_planeZ }, fx, fy)) {
				return false;
			}
			sx[k] = fx;
			sy[k] = fy;
		}
		// Solve for H (screen -> plane): x = (h0 u + h1 v + h2) / (h6 u + h7 v + 1), y = (h3 u + h4 v + h5) / (...)
		double A[8][9]{};
		for (int k = 0; k < 4; ++k) {
			const double u = sx[k], v = sy[k], X = px[k], Yv = py[k];
			double r0[9] = { u, v, 1, 0, 0, 0, -u * X, -v * X, X };
			double r1[9] = { 0, 0, 0, u, v, 1, -u * Yv, -v * Yv, Yv };
			std::copy(r0, r0 + 9, A[2 * k]);
			std::copy(r1, r1 + 9, A[2 * k + 1]);
		}
		for (int c = 0; c < 8; ++c) {
			int piv = c;
			for (int r = c + 1; r < 8; ++r) {
				if (std::fabs(A[r][c]) > std::fabs(A[piv][c])) {
					piv = r;
				}
			}
			if (std::fabs(A[piv][c]) < 1e-12) {
				return false;
			}
			std::swap(A[c], A[piv]);
			for (int r = 0; r < 8; ++r) {
				if (r == c) {
					continue;
				}
				const double f = A[r][c] / A[c][c];
				for (int k = c; k < 9; ++k) {
					A[r][k] -= f * A[c][k];
				}
			}
		}
		double h[8];
		for (int c = 0; c < 8; ++c) {
			h[c] = A[c][8] / A[c][c];
		}
		const double den = h[6] * a_sx + h[7] * a_sy + 1.0;
		if (std::fabs(den) < 1e-12) {
			return false;
		}
		a_x = (h[0] * a_sx + h[1] * a_sy + h[2]) / den;
		a_y = (h[3] * a_sx + h[4] * a_sy + h[5]) / den;
		return true;
	}

	float PixelsPerUnit(const yacht::Vec3& a_at)
	{
		float ax = 0, ay = 0, bx = 0, by = 0;
		if (!Project(a_at, ax, ay) || !Project({ a_at.x + 1.0, a_at.y, a_at.z }, bx, by)) {
			return 20.0f;
		}
		return std::hypot(bx - ax, by - ay);
	}

	void TeardownTick()
	{
		if (g_teardown && TryTeardown()) {
			g_teardown = false;
		}
	}

	std::string Status()
	{
		return fmt::format("scene {} attached {} placed {} scale {:.3f}", g_scene, g_attached, g_placed, g_scale);
	}
}
