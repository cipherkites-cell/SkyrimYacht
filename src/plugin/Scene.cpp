// SkyrimYacht - a dice game for Skyrim Special Edition.
// Copyright (C) 2026 evodube. GPL-3.0 (LICENSE.txt).
#include "Scene.h"

#include <set>

#include "Config.h"
#include "Render3D.h"
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
			const char* fallback = nullptr;  // its path if no item in the game uses it (meshes\\...)
			bool        hideBlood = false;   // the blood chalice: its pool of blood would cover the dice
		};
		constexpr CupModel kCups[] = {
			{ "tankard", "basictankard01.nif", 2.9f, 4.32f, 1.04f },
			{ "bloodtankard", "dlc01tankardblood.nif", 2.9f, 4.32f, 1.04f, "DLC01\\Clutter\\DLC01TankardBlood.nif" },
			{ "glazedcup", "glazedcup01.nif", 2.5f, 4.55f, 1.47f },  // these narrow toward the floor: fit the bottom
			{ "goblet", "glazedgoblet01.nif", 7.6f, 4.4f, 1.47f },
			{ "dwemer", "dwecup01.nif", 0.8f, 4.36f, 1.22f },
			{ "dwemer2", "dwecup02.nif", 1.29f, 4.36f, 1.22f },
			{ "silver", "silvergoblet01.nif", 7.8f, 5.2f, 1.1f },
			{ "silver2", "silvergoblet02.nif", 10.9f, 4.54f, 1.376f },
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
		bool                          g_cupWaiting[2]{};  // a new cup, hidden until it's first put in its place
		std::string              g_diceSet = "bone", g_loadedSet;
		std::string              g_cupId[2] = { "tankard", "tankard" }, g_loadedCup[2];
		int                      g_diceSide = 0;    // which set is showing
		std::unordered_map<std::string, std::string> g_cupPaths;
		RE::NiAVObject*          g_model = nullptr;
		RE::NiPointer<RE::NiAVObject> g_worldTray;  // world mode: our own tray, hung under the world's scene node
		bool                     g_world = false;   // this table is in the world (not the item view)
		bool                     g_overlay = false;  // this table is drawn by us, over the game (Render3D)
		RE::NiPointer<RE::NiNode> g_ovRoot;          // overlay: the table's own little scene (not in the world)
		float                    g_vp[16]{};          // overlay: our camera, table space -> clip
		float                    g_camPos[3]{};
		std::unordered_map<std::string, RE::NiPointer<RE::NiTexture>> g_texCache;  // textures we loaded ourselves
		bool                     g_withTable = true;
		bool                     g_entering = false;  // still rising into place
		bool                     g_hiddenUntilBet = false;
		std::chrono::steady_clock::time_point g_shoutAt{};
		bool                                  g_shoutOn = false;
		float SinceShout() { return g_shoutOn ? std::chrono::duration<float>(std::chrono::steady_clock::now() - g_shoutAt).count() : -1.0f; }

		// The game's own sounds (the fire, the bard, the wind) keep playing while it's paused: hush them at the table.
		std::vector<std::pair<RE::BGSSoundCategory*, float>> g_hushed;
		void HushGame(bool a_hush)
		{
			if (!a_hush) {
				for (auto& [cat, vol] : g_hushed) {
					cat->SetCategoryVolume(vol);
				}
				g_hushed.clear();
				return;
			}
			if (!g_hushed.empty()) {
				return;
			}
			if (auto* dh = RE::TESDataHandler::GetSingleton()) {
				for (auto* cat : dh->GetFormArray<RE::BGSSoundCategory>()) {
					if (cat && !cat->parentCategory) {
						g_hushed.emplace_back(cat, cat->GetCategoryVolume());
						cat->SetCategoryVolume(0.0f);
					}
				}
			}
			spdlog::info("Hushed {} of the game's sound categories", g_hushed.size());
		}
		std::chrono::steady_clock::time_point g_shownAt{};  // the menu opened (the world blurs in)  // the table waits out of sight until the wager is placed
		std::chrono::steady_clock::time_point g_openAt{};  // false: the menu is only a window (the dice shop)

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

		// The preview camera's frame: where it is, and its right / forward / up axes in world space. Gamebryo cameras look
		// down their own X axis (up Y, right Z); rather than trust that, the forward axis is the one pointing at the item
		// the engine itself placed in view.
		struct CamFrame
		{
			RE::NiPoint3 t, right, fwd, up;
			bool         ok = false;
		};
		CamFrame g_cam;

		RE::NiPoint3 Col(const RE::NiMatrix3& m, int c) { return { m.entry[0][c], m.entry[1][c], m.entry[2][c] }; }
		float        Dot(const RE::NiPoint3& a, const RE::NiPoint3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

		CamFrame FindCamera(const RE::NiPoint3& a_itemWorld)
		{
			CamFrame f;
			auto*    scn = RE::UI3DSceneManager::GetSingleton();
			auto*    cam = scn ? scn->camera.get() : nullptr;
			if (!cam) {
				return f;
			}
			const auto& w = cam->world;
			f.t = w.translate;
			const RE::NiPoint3 to = a_itemWorld - f.t;
			int   best = 0;
			float bestDot = 0.0f;
			for (int c = 0; c < 3; ++c) {
				const float d = Dot(Col(w.rotate, c), to);
				if (std::fabs(d) > std::fabs(bestDot)) {
					best = c;
					bestDot = d;
				}
			}
			const float sgn = bestDot < 0.0f ? -1.0f : 1.0f;
			f.fwd = Col(w.rotate, best) * sgn;
			if (best == 0) {  // Gamebryo: forward X, up Y, right Z
				f.up = Col(w.rotate, 1);
				f.right = Col(w.rotate, 2) * sgn;
			} else if (best == 1) {  // Bethesda world: forward Y, up Z, right X
				f.up = Col(w.rotate, 2);
				f.right = Col(w.rotate, 0) * sgn;
			} else {
				f.up = Col(w.rotate, 1);
				f.right = Col(w.rotate, 0) * sgn;
			}
			f.ok = true;
			spdlog::info("Table: preview camera at ({:.1f}, {:.1f}, {:.1f}), forward axis {}{}; item {:.1f} ahead, {:.1f} right, {:.1f} up",
				f.t.x, f.t.y, f.t.z, sgn < 0 ? "-" : "+", best, Dot(f.fwd, to), Dot(f.right, to), Dot(f.up, to));
			return f;
		}

		RE::NiPoint3 Cross(const RE::NiPoint3& a, const RE::NiPoint3& b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }

		// The game's own camera. Forward is the axis whose point ahead lands in the middle of the screen; up is the one
		// nearest the world's up.
		CamFrame FindWorldCamera(RE::NiCamera* a_cam, bool a_log = true)
		{
			CamFrame f;
			if (!a_cam) {
				return f;
			}
			const auto& w = a_cam->world;
			f.t = w.translate;
			int   best = 0;
			float bestSign = 1.0f, bestErr = 1e9f;
			for (int c = 0; c < 3; ++c) {
				for (float sg : { 1.0f, -1.0f }) {
					const RE::NiPoint3 p = f.t + Col(w.rotate, c) * (100.0f * sg);
					float              x = 0, y = 0, z = 0;
					if (!RE::NiCamera::WorldPtToScreenPt3(a_cam->GetRuntimeData().worldToCam, a_cam->GetRuntimeData2().port, p, x, y, z, 1e-5f) || z <= 0.0f) {
						continue;
					}
					const float err = std::fabs(x - 0.5f) + std::fabs(y - 0.5f);
					if (err < bestErr) {
						bestErr = err;
						best = c;
						bestSign = sg;
					}
				}
			}
			if (bestErr > 1e8f) {  // nothing projected: assume Gamebryo's (forward X, up Y, right Z)
				best = 0;
				bestSign = 1.0f;
			}
			f.fwd = Col(w.rotate, best) * bestSign;
			float upZ = 0.0f;
			for (int c = 0; c < 3; ++c) {
				if (c == best) {
					continue;
				}
				const auto v = Col(w.rotate, c);
				if (std::fabs(v.z) > std::fabs(upZ)) {
					upZ = v.z;
					f.up = v.z < 0.0f ? v * -1.0f : v;
				}
			}
			f.right = Cross(f.fwd, f.up);
			f.ok = true;
			if (a_log)
				spdlog::info("Table: game camera at ({:.0f}, {:.0f}, {:.0f}), forward axis {}{} (centre error {:.3f}), forward ({:.2f}, {:.2f}, {:.2f})",
				f.t.x, f.t.y, f.t.z, bestSign < 0 ? "-" : "+", best, bestErr, f.fwd.x, f.fwd.y, f.fwd.z);
			return f;
		}

		bool IsA(RE::NiAVObject* a_obj, std::string_view a_rtti)
		{
			const auto* rtti = a_obj ? a_obj->GetRTTI() : nullptr;
			return rtti && rtti->name && a_rtti == rtti->name;
		}

		// The world's shadow scene node (what the room's lights light): found by climbing from the player's own 3D,
		// or among the world root's children, and only trusted if it really is one.
		RE::NiNode* FindSceneNode()
		{
			// Beside the player: the node that holds the player's own 3D is drawn whenever the player's room is,
			// so the table is too (room-and-portal culling indoors skips nodes outside the rooms).
			if (auto* pc = RE::PlayerCharacter::GetSingleton(); pc && pc->Get3D() && pc->Get3D()->parent) {
				std::string chain;
				for (auto* o = pc->Get3D()->parent; o; o = o->parent) {
					chain += fmt::format(" < {} '{}'", o->GetRTTI() ? o->GetRTTI()->name : "?", o->name.c_str());
				}
				spdlog::info("Table: the player's 3D sits in{}", chain);
				spdlog::info("Table: hung beside the player");
				return pc->Get3D()->parent;
			}
			if (auto* pc = RE::PlayerCharacter::GetSingleton()) {
				for (auto* o = pc->Get3D() ? pc->Get3D()->parent : nullptr; o; o = o->parent) {
					if (IsA(o, "ShadowSceneNode")) {
						spdlog::info("Table: hung under the shadow scene node (from the player)");
						return o;
					}
				}
			}
			if (auto* root = reinterpret_cast<RE::NiNode*>(RE::Main::WorldRootNode())) {  // a Scenegraph is a NiNode
				for (auto& child : root->GetChildren()) {
					if (child && IsA(child.get(), "ShadowSceneNode")) {
						spdlog::info("Table: hung under the shadow scene node (from the world root)");
						return child->AsNode();
					}
				}
				spdlog::warn("Table: no shadow scene node found; hanging it from the world root");
				return root;
			}
			return nullptr;
		}

		// Lifts the table out of a dark room: a little light of its own on every surface.
		void Brighten(RE::NiAVObject* a_obj)
		{
			const float b = Config::Get().brightness;
			if (!a_obj || b <= 1.0f) {
				return;
			}
			const float v = std::clamp((b - 1.0f) * 0.12f, 0.0f, 0.5f);
			RE::BSVisit::TraverseScenegraphGeometries(a_obj, [&](RE::BSGeometry* g) {
				auto* prop = g->GetGeometryRuntimeData().shaderProperty.get();
				if (auto* lit = prop ? netimmerse_cast<RE::BSLightingShaderProperty*>(prop) : nullptr; lit && lit->emissiveColor) {
					lit->emissiveColor->red = v;
					lit->emissiveColor->green = v * 0.92f;
					lit->emissiveColor->blue = v * 0.82f;
					lit->emissiveMult = 1.0f;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
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
			RE::NiPointer<RE::NiAVObject> copy(static_cast<RE::NiAVObject*>(base->Clone()));
			// the game's meshes carry physics bodies; ours are moved by hand
			RE::BSVisit::TraverseScenegraphObjects(copy.get(), [](RE::NiAVObject* a_obj) {
				if (a_obj) {
					a_obj->collisionObject.reset();
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			return copy;
		}

		std::map<std::string, std::vector<RE::TESBoundObject*>> g_cupItems;  // the game's items that are each cup
		std::set<std::string>                                    g_ownedCups;

		void FindCupPaths()
		{
			if (!g_cupPaths.empty()) {
				return;
			}
			auto* dh = RE::TESDataHandler::GetSingleton();
			if (!dh) {
				return;
			}
			// the cups' paths, from the game's own item that uses each mesh
			auto look = [&](const char* model, RE::TESForm* a_form, bool a_item) {
				if (!model || !*model) {
					return;
				}
				std::string m = model;
				std::string low = m;
				std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if (a_item) {  // every drinking vessel in the game, for choosing more cups
					for (const char* w : { "cup", "goblet", "tankard", "mug", "flagon", "chalice", "horn", "stein", "tumbler" }) {
						if (low.find(w) != std::string::npos) {
							spdlog::info("Vessel: {} | {} | {:08X} | {}", a_form->GetName(), m, a_form->GetFormID(),
								a_form->GetFile(0) ? a_form->GetFile(0)->GetFilename() : "?");
							break;
						}
					}
				}
				for (const auto& c : kCups) {
					const auto n = std::strlen(c.file);
					if (a_item && low.size() >= n && low.compare(low.size() - n, n, c.file) == 0) {
						g_cupItems[c.id].push_back(a_form->As<RE::TESBoundObject>());
					}
					if (low.size() >= n && low.compare(low.size() - n, n, c.file) == 0 && (low.size() == n || low[low.size() - n - 1] == '\\' || low[low.size() - n - 1] == '/') &&
						!g_cupPaths.contains(c.id)) {
						g_cupPaths[c.id] = m;
						spdlog::info("Cup {}: {}", c.id, m);
					}
				}
			};
			for (auto* f : dh->GetFormArray<RE::TESObjectMISC>()) {
				if (f) {
					look(f->GetModel(), f, true);
				}
			}
			for (auto* f : dh->GetFormArray<RE::TESObjectSTAT>()) {
				if (f) {
					look(f->GetModel(), f, false);
				}
			}
			for (const auto& c : kCups) {
				if (!g_cupPaths.contains(c.id)) {
					spdlog::warn("Cup {}: nothing in the game uses {}", c.id, c.file);
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
			if (it == g_cupPaths.end() && !c.fallback) {
				spdlog::warn("No item in the game uses {}: the {} won't show", c.file, c.id);
				g_loadedCup[a_which] = g_cupId[a_which];
				return;
			}
			// the game's own paths are relative to Data\Meshes already
			auto mesh = Instance(it != g_cupPaths.end() ? it->second : std::string(c.fallback));
			if (mesh && c.hideBlood) {
				RE::BSVisit::TraverseScenegraphGeometries(mesh.get(), [](RE::BSGeometry* a_geo) {
					if (a_geo && std::string_view(a_geo->name.c_str()).starts_with("Blood")) {
						a_geo->CullGeometry(true);
					}
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
			if (!mesh) {
				g_loadedCup[a_which] = g_cupId[a_which];
				return;
			}
			// the game's cups carry their own physics body: in the running world it would pull the mesh to wherever that
			// (unplaced) body is. Ours moves by hand, so take it off.
			RE::BSVisit::TraverseScenegraphObjects(mesh.get(), [](RE::NiAVObject* a_obj) {
				if (a_obj) {
					a_obj->collisionObject.reset();
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
			// a holder: the cup's origin is the physics cup's (the centre of its inside floor)
			auto holder = RE::NiPointer<RE::NiNode>(RE::NiNode::Create(1));
			const float s = kCupRadius / c.inner * c.room;
			mesh->local.scale = s;
			mesh->local.translate = { 0.0f, 0.0f, -c.floor * s };
			if (a_which == 1) {  // the opponent's: turned about its own axis so the handle points away from the tray (left)
				// a half turn about z on top of the mesh's own rotation (the tankard's root is turned already: writing the
				// two diagonal entries made a skewed matrix that stretched it about 1.4 times)
				for (int c = 0; c < 3; ++c) {
					mesh->local.rotate.entry[0][c] = -mesh->local.rotate.entry[0][c];
					mesh->local.rotate.entry[1][c] = -mesh->local.rotate.entry[1][c];
				}
			}
			holder->AttachChild(mesh.get(), true);
			// hidden until the game puts it where the old one stood (else it shows a frame at the table's middle)
			holder->CullNode(true);
			g_cupWaiting[a_which] = true;
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
			if (g_world || g_overlay) {
				return g_worldTray.get();
			}
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
			if (g_overlay) {
				Render3D::Clear();
				if (g_root && g_worldTray) {
					if (auto* node = g_worldTray->AsNode()) {
						node->DetachChild2(g_root.get());
					}
				}
				if (g_ovRoot && g_worldTray) {
					g_ovRoot->DetachChild2(g_worldTray.get());
				}
				g_worldTray.reset();
				g_ovRoot.reset();
				g_scene = false;
				g_model = nullptr;
				g_overlay = false;
				spdlog::info("Table put away");
				return true;
			}
			if (g_world) {
				if (g_root && g_worldTray) {
					if (auto* node = g_worldTray->AsNode()) {
						node->DetachChild2(g_root.get());
					}
				}
				if (g_worldTray && g_worldTray->parent) {
					g_worldTray->parent->DetachChild2(g_worldTray.get());
				}
				g_worldTray.reset();
				g_scene = false;
				g_model = nullptr;
				g_world = false;
				spdlog::info("Table put away");
				return true;
			}
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
				m->menuFlags.set(Flag::kDisablePauseMenu, Flag::kUsesMenuContext, Flag::kCustomRendering, Flag::kRequiresUpdate);
				// Paused, the world stops drawing new things; the table (in the world) needs it running. Your controls are
				// held by the table either way, and the opponent stays in their seat.
				if (Config::Get().pauseGame || Config::Get().mode != 2) {  // the room table needs the world running
					m->menuFlags.set(Flag::kPausesGame);
				}
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
				if (auto* mgr = RE::Inventory3DManager::GetSingleton(); mgr && g_scene && !g_world) {
					mgr->Render();
				}
			}

		private:
			static void OnShow()
			{
				g_shown = true;
				// the game's HUD (compass, crosshair, the speaker's name) steps aside while you play
				if (auto* ui = RE::UI::GetSingleton(); ui && ui->IsShowingMenus()) {
					ui->ShowMenus(false);
					g_hidHud = true;
				}
				// the line that started the game would otherwise hang on screen while the game is paused
				if (auto* subs = RE::SubtitleManager::GetSingleton()) {
					subs->KillSubtitles();
				}
				g_shownAt = std::chrono::steady_clock::now();
				HushGame(true);
				if (!g_withTable) {
					return;
				}
				if (Config::Get().mode == 0) {
					OpenOverlay();
					return;
				}
				if (Config::Get().mode == 2) {
					OpenWorld();
					return;
				}
				g_blurred = true;
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
				g_shoutOn = false;
				HushGame(false);
				Render3D::Clear();
				if (g_hidHud) {
					g_hidHud = false;
					if (auto* ui = RE::UI::GetSingleton()) {
						ui->ShowMenus(true);
					}
				}
				if (g_blurred) {
					g_blurred = false;
					if (auto* blur = RE::UIBlurManager::GetSingleton()) {
						blur->DecrementBlurCount();
					}
				}
				g_teardown = !TryTeardown();
			}

			// The table as a thing in the room: our tray mesh under the world's scene node, in front of the camera,
			// lit by the room's own lights. The world keeps drawing while the game is paused.
			static void OpenWorld()
			{
				if (g_scene && !TryTeardown()) {
					g_scene = false;
				}
				auto* ssn = FindSceneNode();
				auto  tray = Instance("SkyrimYacht\\Tray.nif");
				if (!ssn || !tray) {
					spdlog::error("Table: couldn't put the tray in the world (scene node {}, mesh {})", ssn != nullptr, tray != nullptr);
					return;
				}
				g_world = true;
				g_worldTray = tray;
				g_openAt = std::chrono::steady_clock::now();
				g_entering = true;
				g_hiddenUntilBet = true;
				tray->CullNode(true);
				g_scene = true;
				g_attached = false;
				g_placed = false;
				g_model = nullptr;
				ssn->AttachChild(tray.get(), true);
				Brighten(tray.get());
				RE::NiUpdateData ud;
				tray->Update(ud);
				spdlog::info("Table: tray placed in the world");
			}

			// The table as the preview draws it: its own little scene, lit and drawn by us over the paused, blurred game.
			static void OpenOverlay()
			{
				if (g_scene && !TryTeardown()) {
					g_scene = false;
				}
				auto tray = Instance("SkyrimYacht\\Tray.nif");
				if (!tray) {
					spdlog::error("Table: couldn't load the tray mesh");
					return;
				}
				g_overlay = true;
				g_ovRoot = RE::NiPointer<RE::NiNode>(RE::NiNode::Create(1));
				g_ovRoot->AttachChild(tray.get(), true);
				g_worldTray = tray;
				g_hiddenUntilBet = true;
				g_openAt = std::chrono::steady_clock::now();
				g_scene = true;
				g_attached = false;
				g_placed = false;
				g_model = nullptr;
				spdlog::info("Table: drawn by the mod over the game");
			}

			static inline bool g_failed = false;
			static inline bool g_blurred = false;
			static inline bool g_hidHud = false;
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

	void Open(bool a_table)
	{
		if (g_open || (a_table && !g_tray)) {
			return;
		}
		g_withTable = a_table;
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
	bool Ready() { return g_withTable && g_scene && g_attached && g_placed; }

	void ShoutFx()
	{
		g_shoutAt = std::chrono::steady_clock::now();
		g_shoutOn = true;
	}

	void Reveal()
	{
		if (!g_hiddenUntilBet) {
			return;
		}
		g_hiddenUntilBet = false;
		g_openAt = std::chrono::steady_clock::now();
		if (g_worldTray) {
			g_worldTray->CullNode(false);
		}
	}

	void UseDice(const std::string& a_set)
	{
		g_diceSet = "bone";
		for (const char* s : kDiceSets) {
			if (a_set == s) {
				g_diceSet = a_set;
			}
		}
	}

	void RefreshOwnedCups()
	{
		FindCupPaths();
		g_ownedCups.clear();
		auto* pc = RE::PlayerCharacter::GetSingleton();
		if (!pc) {
			return;
		}
		const auto counts = pc->GetInventoryCounts([](RE::TESBoundObject& a_obj) { return a_obj.GetFormType() == RE::FormType::Misc; });
		for (const auto& [id, items] : g_cupItems) {
			for (auto* item : items) {
				if (const auto it = counts.find(item); item && it != counts.end() && it->second > 0) {
					g_ownedCups.insert(id);
					break;
				}
			}
		}
		std::string list;
		for (const auto& id : g_ownedCups) {
			list += (list.empty() ? "" : ", ") + id;
		}
		spdlog::info("Cups you carry: {}", list.empty() ? "none (the tankard is always yours)" : list);
	}

	bool OwnsCup(const std::string& a_id) { return g_ownedCups.contains(a_id); }

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
		if (g_cupWaiting[std::clamp(a_which, 0, 1)]) {
			g_cupWaiting[std::clamp(a_which, 0, 1)] = false;
			c->CullNode(false);
		}
	}

	namespace
	{
		ID3D11ShaderResourceView* SrvOf(RE::NiSourceTexture* a_tex)
		{
			// the renderer's texture record is laid out as NiTexture::RendererData (texture, -, view)
			auto* rt = a_tex ? reinterpret_cast<RE::NiTexture::RendererData*>(a_tex->rendererTexture) : nullptr;
			return rt ? reinterpret_cast<::ID3D11ShaderResourceView*>(rt->resourceView) : nullptr;
		}
		// A texture the material hasn't loaded yet: ask the game for it by path (and keep it)
		ID3D11ShaderResourceView* SrvOfPath(const char* a_path)
		{
			if (!a_path || !*a_path) {
				return nullptr;
			}
			auto it = g_texCache.find(a_path);
			if (it == g_texCache.end()) {
				RE::NiPointer<RE::NiTexture> tex;
				RE::BSShaderManager::GetTexture(a_path, true, tex, false);
				it = g_texCache.emplace(a_path, tex).first;
			}
			auto* src = it->second ? netimmerse_cast<RE::NiSourceTexture*>(it->second.get()) : nullptr;
			return SrvOf(src);
		}

		// Each set as the preview draws it (DICE_LOOK): roughness and metalness, and a glow map for the sets that glow
		void DieLook(const std::string& a_set, Render3D::Item& a_it)
		{
			static const std::unordered_map<std::string, std::pair<float, float>> kLook = {
				{ "bone", { 0.8f, 0.0f } }, { "dwemer", { 0.42f, 1.0f } }, { "ebony", { 0.22f, 1.0f } }, { "malachite", { 0.12f, 0.0f } },
				{ "stalhrim", { 0.16f, 0.0f } }, { "dragonbone", { 0.65f, 0.0f } }, { "daedric", { 0.32f, 1.0f } }, { "iron", { 0.62f, 1.0f } },
				{ "steel", { 0.34f, 1.0f } }, { "corundum", { 0.45f, 1.0f } }, { "orichalcum", { 0.5f, 1.0f } }, { "silver", { 0.24f, 1.0f } },
				{ "quicksilver", { 0.06f, 1.0f } }, { "moonstone", { 0.3f, 1.0f } }, { "gold", { 0.24f, 1.0f } },
			};
			const auto it = kLook.find(a_set);
			const auto look = it == kLook.end() ? std::pair{ 0.8f, 0.0f } : it->second;
			a_it.rough = look.first;
			a_it.metal = a_it.envMask ? look.second : 0.0f;  // no mask: no metal in it (as the preview)
			if (a_set == "daedric" || a_set == "stalhrim") {
				a_it.glow = SrvOfPath(("textures\\SkyrimYacht\\Dice_" + a_set + "_g.dds").c_str());
			}
		}

		bool Visible(RE::NiAVObject* a_obj)
		{
			for (auto* o = a_obj; o; o = o->parent) {
				if (o->GetFlags().any(RE::NiAVObject::Flag::kHidden)) {
					return false;
				}
			}
			return true;
		}

		// Our camera, as the preview's: 38 degrees tall, looking down on the tray at the set tilt, far enough back to fit
		// the tray and both cups across the set share of the screen.
		void Camera(float a_reveal)
		{
			(void)a_reveal;
			const auto& cfg = Config::Get();
			const float aspect = g_screenW / (std::max)(1.0f, g_screenH);
			// the Shout's blast punches the lens wide for a moment and shakes the view (as the preview)
			float kick = 0.0f, shake = 0.0f;
			if (const float s = SinceShout(); s >= 0.0f) {
				kick = s < 0.07f ? (s / 0.07f) * 3.2f : (s < 0.9f ? 3.2f * std::exp(-(s - 0.07f) * 7.0f) * std::cos((s - 0.07f) * 14.0f) : 0.0f);
				const float st = (std::max)(0.0f, 0.55f - s);
				shake = st * st * 1.1f;
			}
			const float vfov = (38.0f + kick) * 0.0174533f, th = std::tan(vfov * 0.5f);
			const float needW = 2.0f * (12.65f + 0.8f + 3.6f + 1.9f + 3.0f), needD = 31.0f, zc = 1.8f;
			const float fit = (std::max)(0.3f, cfg.fitWidth);
			const float dist = (std::max)((needD * 0.5f) / th, (needW * 0.5f) / (th * aspect)) * 0.95f / fit;
			const float tilt = std::clamp(cfg.tiltDeg, 15.0f, 89.0f) * 0.0174533f;
			float eye[3] = { 0.0f, -(std::cos(tilt) * dist + zc), std::sin(tilt) * dist };
			if (shake > 0.0f) {
				eye[0] += (static_cast<float>(std::rand()) / RAND_MAX - 0.5f) * shake;
				eye[1] += (static_cast<float>(std::rand()) / RAND_MAX - 0.5f) * shake;
			}
			const float at[3] = { 0.0f, -zc, 0.0f };
			float f[3] = { at[0] - eye[0], at[1] - eye[1], at[2] - eye[2] };
			const float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
			for (auto& v : f) {
				v /= fl;
			}
			const float up[3] = { 0, 0, 1 };
			float r[3] = { f[1] * up[2] - f[2] * up[1], f[2] * up[0] - f[0] * up[2], f[0] * up[1] - f[1] * up[0] };
			const float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
			for (auto& v : r) {
				v /= rl;
			}
			const float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
			auto dot = [](const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
			const float view[16] = { r[0], r[1], r[2], -dot(r, eye), u[0], u[1], u[2], -dot(u, eye), -f[0], -f[1], -f[2], dot(f, eye), 0, 0, 0, 1 };
			const float n = 0.5f, fa = 400.0f;
			float proj[16] = { 1.0f / (th * aspect), 0, 0, 0, 0, 1.0f / th, 0, 0, 0, 0, fa / (n - fa), n * fa / (n - fa), 0, 0, -1, 0 };
			// where its middle sits on screen (the preview draws it a little low, clear of the scorecards)
			const float ox = (cfg.centerX - 0.5f) * 2.0f, oy = -(cfg.centerY - 0.5f) * 2.0f;
			for (int k = 0; k < 4; ++k) {
				proj[0 + k] += ox * proj[12 + k];
				proj[4 + k] += oy * proj[12 + k];
			}
			for (int i = 0; i < 4; ++i) {
				for (int j = 0; j < 4; ++j) {
					float sum = 0;
					for (int k = 0; k < 4; ++k) {
						sum += proj[i * 4 + k] * view[k * 4 + j];
					}
					g_vp[i * 4 + j] = sum;
				}
			}
			std::memcpy(g_camPos, eye, sizeof(eye));
		}

		void UpdateOverlay(float a_sinceShown)
		{
			// the size of the picture we draw into and the mouse moves over (the back buffer): the window's own size can
			// differ (upscalers, resolution scaling, borderless at another size), which put the dice's hit boxes off them
			Overlay::ScreenSize(g_screenW, g_screenH);
			auto* model = g_worldTray.get();
			if (!g_ovRoot || !model) {
				return;
			}
			model->local = RE::NiTransform();
			// it rises into place once the wager is placed
			const float t = g_hiddenUntilBet ? 0.0f : std::clamp(std::chrono::duration<float>(std::chrono::steady_clock::now() - g_openAt).count() / 0.9f, 0.0f, 1.0f);
			const float e = 1.0f - std::pow(1.0f - t, 3.0f);
			g_ovRoot->local.translate = { 0.0f, 0.0f, -(1.0f - e) * 7.0f };
			RE::NiUpdateData ud;
			g_ovRoot->Update(ud);
			Camera(e);
			if (!g_placed) {
				spdlog::info("Table: overlay camera at ({:.1f}, {:.1f}, {:.1f}) for {}x{}", g_camPos[0], g_camPos[1], g_camPos[2], g_screenW, g_screenH);
			}
			g_placed = true;

			Render3D::Frame f;
			f.show = true;
			f.reveal = g_hiddenUntilBet ? 0.0f : e;
			f.blur = std::clamp(a_sinceShown / 0.4f, 0.0f, 1.0f);
			if (const float s = SinceShout(); s >= 0.0f && s <= 1.05f) {
				f.shout = s;
			} else {
				g_shoutOn = false;
			}
			f.aspect = g_screenW / (std::max)(1.0f, g_screenH);
			std::memcpy(f.viewProj, g_vp, sizeof(g_vp));
			std::memcpy(f.camPos, g_camPos, sizeof(g_camPos));
			int skipped = 0;
			if (!g_hiddenUntilBet) {
				RE::BSVisit::TraverseScenegraphGeometries(g_ovRoot.get(), [&](RE::BSGeometry* a_geo) {
					auto* tri = a_geo ? a_geo->AsTriShape() : nullptr;
					if (!tri || !Visible(a_geo)) {
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					auto&       gd = a_geo->GetGeometryRuntimeData();
					auto*       rd = gd.rendererData;
					auto*       prop = gd.shaderProperty.get();
					auto*       lit = prop ? netimmerse_cast<RE::BSLightingShaderProperty*>(prop) : nullptr;
					auto*       mat = lit ? static_cast<RE::BSLightingShaderMaterialBase*>(lit->material) : nullptr;
					if (!rd || !rd->vertexBuffer || !rd->indexBuffer || !mat) {
						++skipped;
						return RE::BSVisit::BSVisitControl::kContinue;
					}
					Render3D::Item it;
					it.vb = reinterpret_cast<::ID3D11Buffer*>(rd->vertexBuffer);
					it.ib = reinterpret_cast<::ID3D11Buffer*>(rd->indexBuffer);
					static_assert(sizeof(gd.vertexDesc) == sizeof(std::uint64_t));
					// the renderer's own record of the layout (what the vertex buffer really holds)
					std::memcpy(&it.desc, &rd->vertexDesc, sizeof(it.desc));
					it.indexCount = static_cast<std::uint32_t>(tri->GetTrishapeRuntimeData().triangleCount) * 3;
					it.vertexCount = tri->GetTrishapeRuntimeData().vertexCount;
					{
						std::uint64_t geoDesc = 0;
						std::memcpy(&geoDesc, &gd.vertexDesc, sizeof(geoDesc));
						it.geoDesc = geoDesc;
					}
					it.name = a_geo->name.c_str();
					const auto& w = a_geo->world;
					for (int rr = 0; rr < 3; ++rr) {
						for (int cc = 0; cc < 3; ++cc) {
							it.rot[rr * 3 + cc] = w.rotate.entry[rr][cc];
						}
					}
					it.pos[0] = w.translate.x;
					it.pos[1] = w.translate.y;
					it.pos[2] = w.translate.z;
					it.scale = w.scale;
					auto* set = mat->textureSet.get();
					it.diffuse = SrvOf(mat->diffuseTexture.get());
					if (!it.diffuse && set) {
						it.diffuse = SrvOfPath(set->GetTexturePath(RE::BSTextureSet::Texture::kDiffuse));
					}
					it.normal = SrvOf(mat->normalTexture.get());
					if (!it.normal && set) {
						it.normal = SrvOfPath(set->GetTexturePath(RE::BSTextureSet::Texture::kNormal));
					}
					if (mat->GetFeature() == RE::BSShaderMaterial::Feature::kEnvironmentMap) {
						auto* env = static_cast<RE::BSLightingShaderMaterialEnvmap*>(mat);
						it.env = SrvOf(env->envTexture.get());
						it.envMask = SrvOf(env->envMaskTexture.get());
						it.envScale = env->envMapScale;
					}
					const bool specular = lit->flags.any(RE::BSShaderProperty::EShaderPropertyFlag::kSpecular);
					it.specColor[0] = mat->specularColor.red;
					it.specColor[1] = mat->specularColor.green;
					it.specColor[2] = mat->specularColor.blue;
					it.specScale = specular ? mat->specularColorScale : 0.0f;
					it.gloss = mat->specularPower;
					it.alpha = mat->materialAlpha;
					if (auto* ap = gd.alphaProperty.get()) {
						if (auto* alpha = ap) {
							it.blend = alpha->GetAlphaBlending();
							it.test = alpha->GetAlphaTesting();
							it.alphaRef = alpha->alphaThreshold / 255.0f;
						}
					}
					if (it.name == "Die") {  // the dice: the preview's material, metal where the mask says
						DieLook(g_loadedSet, it);
					}
					f.items.push_back(it);
					return RE::BSVisit::BSVisitControl::kContinue;
				});
			}
			static bool s_logged = false;
			if (!s_logged && !f.items.empty()) {
				s_logged = true;
				int tex = 0;
				for (const auto& it : f.items) {
					tex += it.diffuse != nullptr;
				}
				spdlog::info("Table: {} meshes to draw ({} with their textures), {} skipped", f.items.size(), tex, skipped);
			}
			Render3D::Submit(std::move(f));
		}
	}

	void Update()
	{
		const float sinceShown = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_shownAt).count();
		if (g_open && !g_withTable) {  // the dice shop: just the world blurred behind it
			Render3D::Frame f;
			f.show = true;
			f.blur = std::clamp(sinceShown / 0.4f, 0.0f, 1.0f);
			Render3D::Submit(std::move(f));
			return;
		}
		auto* mgr = RE::Inventory3DManager::GetSingleton();
		if (!mgr || !g_scene) {
			return;
		}
		auto* model = TrayModel(mgr);
		if (!model || (!g_world && !g_overlay && model->worldBound.radius <= 0.0f)) {
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

		if (g_overlay) {
			UpdateOverlay(sinceShown);
			return;
		}
		// Place the table: tilted toward you, scaled to fit the screen, centred where the settings say.
		if (g_world) {
			if (auto* cam = RE::Main::WorldRootCamera()) {
				g_frustum = cam->GetRuntimeData2().viewFrustum;
			}
		} else if (auto* scn = RE::UI3DSceneManager::GetSingleton()) {
			g_frustum = scn->viewFrustum;
		}
		if (g_overlay) {
			Overlay::ScreenSize(g_screenW, g_screenH);
		} else if (auto* r = RE::BSGraphics::Renderer::GetSingleton()) {
			const auto& win = r->GetRuntimeData().renderWindows[0];
			g_screenW = static_cast<float>(win.windowWidth > 0 ? win.windowWidth : 1920);
			g_screenH = static_cast<float>(win.windowHeight > 0 ? win.windowHeight : 1080);
		}
		const auto& cfg = Config::Get();
		static float s_depth = 0.0f;
		if (!g_placed && g_world) {
			g_cam = FindWorldCamera(RE::Main::WorldRootCamera());
			s_depth = cfg.distance;
			spdlog::info("Table: frustum L {:.3f} R {:.3f} T {:.3f} B {:.3f}", g_frustum.fLeft, g_frustum.fRight, g_frustum.fTop, g_frustum.fBottom);
		} else if (g_world) {
			// the world keeps running: follow the camera if it drifts (idle sway), so the table stays put on screen
			if (auto fresh = FindWorldCamera(RE::Main::WorldRootCamera(), false); fresh.ok) {
				g_cam = fresh;
			}
		} else if (!g_placed) {
			g_cam = FindCamera(model->world.translate);
			const float ahead = g_cam.ok ? Dot(g_cam.fwd, model->world.translate - g_cam.t) : 0.0f;
			s_depth = cfg.depth > 0.0f ? cfg.depth : (ahead > 1.0f ? ahead : 100.0f);
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
		const float rz = (g_frustum.fTop - g_frustum.fBottom) * d / g_screenH;
		g_pos = { -(cfg.centerX * g_screenW) * rx + g_frustum.fLeft * d, d, g_frustum.fTop * d - (cfg.centerY * g_screenH) * rz };
		if (g_world) {
			// it rises into place as you sit down, rather than appearing at once
			const float t = g_hiddenUntilBet ? 0.0f : std::clamp(std::chrono::duration<float>(std::chrono::steady_clock::now() - g_openAt).count() / 0.9f, 0.0f, 1.0f);
			const float e = 1.0f - std::pow(1.0f - t, 3.0f);
			g_pos.z -= (1.0f - e) * d * 0.45f;
			g_pos.y += (1.0f - e) * d * 0.15f;
			g_entering = t < 1.0f;
		}
		// camera space (x right, y ahead, z up) -> world -> the model's parent
		{
			RE::NiMatrix3 basis;  // columns: right, ahead, up
			for (int r = 0; r < 3; ++r) {
				basis.entry[r][0] = (&g_cam.right.x)[r];
				basis.entry[r][1] = (&g_cam.fwd.x)[r];
				basis.entry[r][2] = (&g_cam.up.x)[r];
			}
			if (!g_cam.ok) {
				basis = RE::NiMatrix3();
			}
			const RE::NiMatrix3 wRot = basis * g_rot;
			const RE::NiPoint3  wPos = g_cam.t + Mul(basis, g_pos);
			RE::NiTransform     parent;
			if (auto* p = model->parent) {
				parent = p->world;
			}
			const RE::NiMatrix3 pInv = parent.rotate.Transpose();
			const float         ps = parent.scale != 0.0f ? parent.scale : 1.0f;
			model->local.rotate = pInv * wRot;
			model->local.scale = g_scale / ps;
			model->local.translate = Mul(pInv, wPos - parent.translate) / ps;
		}
		if (g_world) {
			// a mesh new to the world fades in with game time, which is stopped while the game is paused: show it now
			RE::BSVisit::TraverseScenegraphObjects(model, [](RE::NiAVObject* a_obj) {
				if (auto* fade = a_obj ? a_obj->AsFadeNode() : nullptr) {
					fade->GetRuntimeData().currentFade = 1.0f;
				}
				return RE::BSVisit::BSVisitControl::kContinue;
			});
		}
		RE::NiUpdateData ud;
		model->Update(ud);
		if (!g_placed && g_world) {
			const auto& wb = model->worldBound;
			spdlog::info("Table: world bound centre ({:.0f}, {:.0f}, {:.0f}) radius {:.1f}, in view {}", wb.center.x, wb.center.y, wb.center.z, wb.radius,
				RE::Main::WorldRootCamera() ? RE::Main::WorldRootCamera()->NodeInFrustum(model) : false);
		}
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
		if (g_overlay) {
			const float x = static_cast<float>(a_p.x), y = static_cast<float>(a_p.y), z = static_cast<float>(a_p.z);
			const float cx = g_vp[0] * x + g_vp[1] * y + g_vp[2] * z + g_vp[3];
			const float cy = g_vp[4] * x + g_vp[5] * y + g_vp[6] * z + g_vp[7];
			const float cw = g_vp[12] * x + g_vp[13] * y + g_vp[14] * z + g_vp[15];
			if (cw <= 0.01f) {
				return false;
			}
			a_sx = (cx / cw * 0.5f + 0.5f) * g_screenW;
			a_sy = (0.5f - cy / cw * 0.5f) * g_screenH;
			return std::isfinite(a_sx) && std::isfinite(a_sy);
		}
		const RE::NiPoint3 local{ static_cast<float>(a_p.x) * g_scale, static_cast<float>(a_p.y) * g_scale, static_cast<float>(a_p.z) * g_scale };
		const RE::NiPoint3 w = Mul(g_rot, local) + g_pos;
		if (w.y <= 0.01f) {
			return false;
		}
		a_sx = (g_frustum.fLeft * w.y - w.x) / ((g_frustum.fLeft - g_frustum.fRight) * w.y) * g_screenW;
		a_sy = (g_frustum.fTop * w.y - w.z) / ((g_frustum.fTop - g_frustum.fBottom) * w.y) * g_screenH;
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
