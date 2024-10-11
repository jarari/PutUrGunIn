#include "include/detours.h"
#include <SimpleIni.h>
#include <fstream>
#include <shared_mutex>
#include <wtypes.h>
using namespace RE;

struct ObjectEquipParams {
	uint32_t a_stackID;
	uint32_t a_number;
};

REL::Relocation<uintptr_t> ActorEquipManager_EquipOrig{ REL::ID(1474878) };
REL::Relocation<float*> ptr_engineTime{ REL::ID(599343) };

std::atomic<bool> threadLaunched = false;
std::atomic<bool> abortThread = false;
std::atomic<bool> equipLocked = false;
uint32_t targetFormID = 0x0;

void GetClipInfo(Actor* actor, float& currentTime, float& duration, std::string& clipName)
{
	if (actor->currentProcess) {
		RE::BSAnimationGraphManager* graphManager = actor->currentProcess->middleHigh->animationGraphManager.get();
		if (graphManager) {
			RE::BShkbAnimationGraph* graph = graphManager->variableCache.graphToCacheFor.get();
			if (graph) {
				void* hkGraph = *(void**)((uintptr_t)graph + 0x378);
				typedef RE::hkArray<void**, struct hkContainerHeapAllocator> nodeArray;
				nodeArray* activeNodes = *(nodeArray**)((uintptr_t)hkGraph + 0xE0);
				if (activeNodes && activeNodes->_size > 0) {
					void** generatorArray = *activeNodes->_data;
					while (*generatorArray) {
						uintptr_t clip = (uintptr_t)(*generatorArray);
						if (*(uint32_t*)(clip + 0x8)) {
							currentTime = *(float*)(clip + 0x140);
							clipName = std::string(*(const char**)(clip + 0x38));
							if (currentTime) {
								uintptr_t animCtrl = *(uintptr_t*)(clip + 0xD0);
								if (animCtrl) {
									uintptr_t animBinding = *(uintptr_t*)(animCtrl + 0x38);
									uintptr_t anim = *(uintptr_t*)(animBinding + 0x18);
									duration = *(float*)(anim + 0x14);
									return;
								}
							}
						}
						generatorArray = (void**)((uintptr_t)generatorArray + 0x8);
					}
				}
			}
		}
	}
}

bool HookedEquip(ActorEquipManager* a_manager, Actor* a_actor, BGSObjectInstanceT<TESBoundObject> a_obj, ObjectEquipParams& a_params)
{
	typedef bool (*FnUnequipSlot)(ActorEquipManager*, Actor*, BGSObjectInstanceT<TESBoundObject>, ObjectEquipParams&);
	FnUnequipSlot fn = (FnUnequipSlot)ActorEquipManager_EquipOrig.address();

	if (a_actor == PlayerCharacter::GetSingleton() && a_obj.object->formType == ENUM_FORM_ID::kWEAP) {
		if (UI::GetSingleton()->GetMenuOpen("PipboyMenu")) {
			abortThread = true;
		} else {
			TESObjectWEAP* a_wep = static_cast<TESObjectWEAP*>(a_obj.object);
			if (a_wep->weaponData.type != WEAPON_TYPE::kGrenade && a_wep->weaponData.type != WEAPON_TYPE::kMine && !(a_wep->weaponData.flags & 0x1A0000)) {	//Only run if the weapon has none of those flags set: Can't Drop, Not Playable, Embed Weapon
				//logger::info("Equip obj formID {:04X} flags {:04X}", a_obj.object->formID, a_wep->weaponData.flags);
				if (equipLocked && a_actor->weaponState <= WEAPON_STATE::kDrawing && targetFormID == a_obj.object->formID) {
					bool expected = false;
					if (threadLaunched.compare_exchange_strong(expected, true)) {
						a_actor->weaponState = WEAPON_STATE::kDrawing;
						//logger::info("Equip thread launch");
						std::jthread([a_actor, a_obj]() {
							float currentTime = 0.f, duration = FLT_MAX;
							std::string clipName = "";
							GetClipInfo(a_actor, currentTime, duration, clipName);
							while ((!abortThread && a_actor && a_actor->currentProcess) &&
								(a_actor->currentProcess->middleHigh->requestedWeaponSubGraphID.size() ||
								a_actor->weaponState == WEAPON_STATE::kDrawing ||
								(a_actor->weaponState == WEAPON_STATE::kDrawn && duration - currentTime >= 0.7f))) {
								//logger::info("requested ID {:08X} time left {}", a_actor->currentProcess->middleHigh->requestedWeaponSubGraphID[1].identifier, duration - currentTime);
								GetClipInfo(a_actor, currentTime, duration, clipName);
								std::this_thread::sleep_for(std::chrono::milliseconds(50));
							}
							//logger::info("Equip complete");
							targetFormID = 0x0;
							equipLocked = false;
							threadLaunched = false;
						}).detach();
					}
				} else if (!equipLocked && a_actor->weaponState == WEAPON_STATE::kDrawn) {
					bool expected = false;
					if (threadLaunched.compare_exchange_strong(expected, true)) {
						//logger::info("Unequip thread launch");
						abortThread = false;
						if (a_actor->NotifyAnimationGraphImpl("Unequip")) {
							equipLocked = true;
							targetFormID = a_obj.object->formID;
							std::jthread([a_actor, a_obj]() {
								while (!abortThread && a_actor && a_actor->weaponState != WEAPON_STATE::kSheathed) {
									std::this_thread::sleep_for(std::chrono::milliseconds(10));
								}
								//logger::info("Unequip done");
								threadLaunched = false;
								F4SE::GetTaskInterface()->AddTask([a_actor, a_obj]() {
									if (!abortThread) {
										//logger::info("Equip item");
										if (a_actor->currentProcess) {
											*(bool*)((uintptr_t)a_actor->currentProcess->high + 0x57B) = true;
										}
										ActorEquipManager::GetSingleton()->EquipObject(a_actor, a_obj, 0, 1, nullptr, true, false, false, false, false);
									}
								});
							}).detach();
							return false;
						} else {
							//logger::info("Unequip not fired");
							abortThread = true;
							equipLocked = false;
							threadLaunched = false;
							targetFormID = 0x0;
							float currentTime = 0.f, duration = FLT_MAX;
							std::string clipName = "";
							GetClipInfo(a_actor, currentTime, duration, clipName);
							if (clipName == "WPNEquip" || clipName == "WPNEquipFast") {
								//logger::info("Clip was equip");
								return false;
							}
						}
					}
				} else if (equipLocked) {
					//logger::info("Equip locked");
					return false;
				}
			}
		}
	}
	if (fn)
		return (*fn)(a_manager, a_actor, a_obj, a_params);
	return false;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Query(const F4SE::QueryInterface * a_f4se, F4SE::PluginInfo * a_info) {
#ifndef NDEBUG
	auto sink = std::make_shared<spdlog::sinks::msvc_sink_mt>();
#else
	auto path = logger::log_directory();
	if (!path) {
		return false;
	}

	*path /= fmt::format(FMT_STRING("{}.log"), Version::PROJECT);
	auto sink = std::make_shared<spdlog::sinks::basic_file_sink_mt>(path->string(), true);
#endif

	auto log = std::make_shared<spdlog::logger>("global log"s, std::move(sink));

#ifndef NDEBUG
	log->set_level(spdlog::level::trace);
#else
	log->set_level(spdlog::level::info);
	log->flush_on(spdlog::level::info);
#endif

	spdlog::set_default_logger(std::move(log));
	spdlog::set_pattern("[%^%l%$] %v"s);

	logger::info(FMT_STRING("{} v{}"), Version::PROJECT, Version::NAME);

	a_info->infoVersion = F4SE::PluginInfo::kVersion;
	a_info->name = Version::PROJECT.data();
	a_info->version = Version::MAJOR;

	if (a_f4se->IsEditor()) {
		logger::critical(FMT_STRING("loaded in editor"));
		return false;
	}

	const auto ver = a_f4se->RuntimeVersion();
	if (ver < F4SE::RUNTIME_1_10_162) {
		logger::critical(FMT_STRING("unsupported runtime v{}"), ver.string());
		return false;
	}

	F4SE::AllocTrampoline(8 * 8);

	return true;
}

extern "C" DLLEXPORT bool F4SEAPI F4SEPlugin_Load(const F4SE::LoadInterface * a_f4se) {
	F4SE::Init(a_f4se);

	DetourTransactionBegin();
	DetourUpdateThread(GetCurrentThread());
	DetourAttach(&(PVOID&)ActorEquipManager_EquipOrig, HookedEquip);
	DetourTransactionCommit();

	const F4SE::MessagingInterface* message = F4SE::GetMessagingInterface();
	message->RegisterListener([](F4SE::MessagingInterface::Message* msg) -> void {
		if (msg->type == F4SE::MessagingInterface::kPostLoadGame || msg->type == F4SE::MessagingInterface::kNewGame) {
			abortThread = true;
			equipLocked = false;
			targetFormID = 0x0;
		}
	});

	return true;
}
