// The mod's own menus in a mission: the ability menu (numpad .) and the
// announcement list (Insert). See menus.h.

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include "menus.h"
#include "strings.h"
#include "abar.h"
#include "history.h"
#include "game.h"
#include "log.h"
#include "speech.h"
#include "ue3.h"
#include "input.h"
#include "cursor.h"

// Keys still owed to a mod menu that just closed: the key that closed it has
// events left to come, and they must not reach whatever is underneath.
static ULONGLONG g_menu_grace_until;

// ---- the ability menu ------------------------------------------------------
//
// Numpad . opens the ability bar as a menu: numpad 8 / 2 or the Up / Down
// arrows walk it, numpad 5 says the entry again, numpad . or Escape closes
// it. Each entry is the key and name, the status, and the ability's own
// tooltip -- XGAbility.strHelp, what GetHelpText returns and the info panel
// shows while aiming (SetHelp). It is read off the container's
// m_arrAbilities, the same array PopulateFlash drew slot by slot.
//
// Decimal is bound to nothing in a mission -- DefaultInput.ini names it only
// in the edit-box alias lists, as it does * and /. The arrows and Escape are
// bound (Arrow_Up .. InputEvent 500-503, Escape 510), so while the menu is
// open they are kept from the game in hook_moviecheck, the same way End is.
static FieldSlot g_bar_abilities, g_ability_help;

// What an ability costs in ammo, asked of the game: the cost
// XGAbilityTree.ApplyActionCost takes off is GetAmmoCost(weapon type,
// ability type, aUpgrades[ePerk_Foundry_AmmoConservation] > 0, the
// character, reaction), a native. Read from the exe (HANDOFF, "The ammo
// costs"), it depends on the weapon's Heavy and MEC properties, the ability,
// a MEC's Expanded Storage and Ammo Conservation -- so it is asked rather
// than copied. A MEC's Fire costs 50, its reaction shot 33, Collateral
// Damage the whole clip.
//
// Rapid Fire's two shots each pay: the weapon panel shows it doubled
// (UITacticalHUD_WeaponPanel.SetWeaponAndAmmo, GetType() == 8), and a
// Scatter Laser at 50 was empty after one (2026-10-07 log).
//
// Overwatch costs nothing itself; what it costs is the reaction shot it
// fires later, which is a standard shot with bReactionFire set
// (XGUnit's ammo check passes bReactionShot the same way). Its ability may
// carry no weapon, so then the unit's primary one is asked about.
//
// Returns the percentage of the clip, 0 for none, -1 when it cannot be told.
#define ABILITY_SHOT_STANDARD 7     // eAbility_ShotStandard
#define ABILITY_RAPID_FIRE    8     // eAbility_RapidFire
#define ABILITY_OVERWATCH     22    // eAbility_Overwatch
#define PERK_AMMO_CONSERVATION 113  // ePerk_Foundry_AmmoConservation
static FieldSlot g_ab_type, g_ab_core, g_ab_unit, g_ab_weapon, g_ab_inventory,
                 g_ab_primary, g_ab_item_type, g_ab_character;

static void* object_at(void* obj, const char* name, FieldSlot* slot)
{
    const void* v;
    if (!obj || !field_ptr(obj, name, slot, sizeof(void*), &v)) return NULL;
    void* o = *(void* const*)v;
    return o && unit_is_live(o) ? o : NULL;
}

static int ability_ammo_cost(void* ability, int* type_out)
{
    const void* v;
    *type_out = -1;
    if (g_core_slot_ammocost < 0 ||
        !field_ptr(ability, "iType", &g_ab_type, sizeof(int), &v))
        return -1;
    int type = *type_out = *(const int*)v;
    void* core = object_at(ability, "m_kGameCore", &g_ab_core);
    void* unit = object_at(ability, "m_kUnit", &g_ab_unit);
    if (!core || !unit) return -1;
    // m_kWeapon is XGAbility_Targeted's; a plain XGAbility uses no weapon.
    void* weapon = NULL;
    uint32_t weapon_off;
    if (object_field_offset(ability, "m_kWeapon", &weapon_off))
        weapon = object_at(ability, "m_kWeapon", &g_ab_weapon);
    if (!weapon && type == ABILITY_OVERWATCH)
        weapon = object_at(object_at(unit, "m_kInventory", &g_ab_inventory),
                           "m_kPrimaryWeapon", &g_ab_primary);
    if (!weapon) return 0;
    if (!field_ptr(weapon, "m_eType", &g_ab_item_type, 1, &v)) return -1;
    int item = *(const uint8_t*)v;

    void* character = object_at(unit, "m_kCharacter", &g_ab_character);
    uint32_t char_off = 0, up_off = 0;
    const void* st = character ? field_struct(character, "m_kChar", &char_off) : NULL;
    if (!st || !struct_member(st, "aUpgrades", &up_off, NULL)) return -1;
    const uint8_t* kchar = (const uint8_t*)character + char_off;
    const int* conserve_at = (const int*)(kchar + up_off) + PERK_AMMO_CONSERVATION;
    if (!readable(kchar, 0x600) || !readable(conserve_at, sizeof(int))) return -1;
    int conserve = *conserve_at > 0;

    AmmoCostFn cost_of = (AmmoCostFn)tile_vfn(core, g_core_slot_ammocost);
    if (!cost_of) return -1;
    int reaction = type == ABILITY_OVERWATCH;
    int cost = cost_of(core, NULL, item, reaction ? ABILITY_SHOT_STANDARD : type,
                       conserve, kchar, reaction);
    if (type == ABILITY_RAPID_FIRE) cost *= 2;
    logf_("abar: ammo cost of ability %d with item %d%s: %d\n", type, item,
          reaction ? " (its reaction shot)" : "", cost);
    return cost < 0 ? -1 : cost;
}

// " Uses 50% ammo." for an ability that spends any; "" otherwise.
static void ability_cost_words(void* ability, char* out, size_t out_sz)
{
    out[0] = 0;
    int type = -1, cost = -1;
    Fault f;
    __try {
        cost = ability_ammo_cost(ability, &type);
    } __except (fault_note(GetExceptionInformation(), &f)) {
        fault_log("abar: ammo cost", &f, NULL);
        cost = -1;
    }
    if (cost <= 0) return;
    tfmt(out, out_sz, type == ABILITY_OVERWATCH ? MENU_REACTION_AMMO : MENU_USES_AMMO, cost);
}

static void abar_help(int index, char* out, size_t out_sz)
{
    out[0] = 0;
    const void* v;
    void* bar = abar_container();
    if (!bar || !unit_is_live(bar) ||
        !field_ptr(bar, "m_arrAbilities", &g_bar_abilities, sizeof(FArray), &v))
        return;
    const FArray* a = (const FArray*)v;
    if (index < 0 || index >= a->Num || a->Num > 64 ||
        !readable(a->Data, (size_t)a->Num * sizeof(void*)))
        return;
    void* ability = ((void* const*)a->Data)[index];
    if (!ability || !unit_is_live(ability)) return;
    if (field_ptr(ability, "strHelp", &g_ability_help, sizeof(FString), &v) &&
        read_fstring((const FString*)v, out, out_sz))
        strip_markup(out);
    char cost[160];
    ability_cost_words(ability, cost, sizeof cost);
    if (cost[0]) {
        size_t used = strlen(out);
        // No help text: the cost alone, without its leading space.
        _snprintf_s(out + used, out_sz - used, _TRUNCATE, "%s", used ? cost : cost + 1);
    }
}

static void abar_say_entry(void)
{
    char help[512], say[1024];
    int i = abar_menu_index();
    abar_help(i, help, sizeof help);
    if (!abar_entry(i, help, say, sizeof say))
        strncpy_s(say, sizeof say, T(MENU_NO_ABILITIES), _TRUNCATE);
    logf_("abar: %d of %d \"%s\"\n", i + 1, abar_count(), say);
    speech_cancel_pending();
    speech_say_now(say);
}

// The menu's keys, as last seen: numpad ., numpad 8, numpad 2, numpad 5, Up,
// Down, Escape, Enter, Space, and the key two right of P, which opens and
// closes the menu as numpad . does on a keyboard without a numpad.
//
// That last one is taken by where it is, not by what it types (input.h,
// input_key_at): scan 0x1B, ] on a US layout, u acute on a Hungarian one, +
// on a German one. None of the names UE3 gives what it can be -- RightBracket,
// Equals, Semicolon and the like -- is bound in [XComGame.XComTacticalInput],
// so it never becomes an InputEvent and nothing has to be swallowed.
#define MENU_KEYS 10
#define MENU_KEY_ALT   9
#define MENU_ALT_SCAN  0x1B
static const int g_menu_vk[MENU_KEYS] = {
    VK_DECIMAL, VK_NUMPAD8, VK_NUMPAD2, VK_NUMPAD5, VK_UP, VK_DOWN, VK_ESCAPE,
    VK_RETURN, VK_SPACE, 0
};
static int g_menu_was[MENU_KEYS];

// Enter uses the ability: it becomes the ability's own number key.
//
// In a mission Enter and Space both raise InputEvent(513) (Spacebar_Key_Press;
// Enter's binding in [XComGame.XComTacticalInput] is the same command), and
// the number keys 1-0 raise 612-621, which
// UITacticalHUD_AbilityContainer.OnUnrealCommand turns into
// DirectPickAbility(0..9) -- on the key's RELEASE, since it returns early for
// anything without mask 32. So nothing is synthesised: the Enter keystroke's
// own two events are rewritten in InputEvent's frame, press and release, from
// 513 to 612 + index, and the game does exactly what that number key does.
//
// The rewrite has to happen before InputEvent looks at Cmd at all, and in EW
// that is earlier than IsAnyMoviePlaying. EW's InputEvent opens with
// PreProcessEventMatching, which drops a release unless m_arrEventTrackers
// holds a press of the same Cmd, and it closes with ActivateTracker(Cmd), which
// records the press under whatever Cmd has become by then. Rewritten at
// IsAnyMoviePlaying, the press was recorded as the number key, Enter's release
// was dropped before the hook ever saw it, and the number key stayed "held":
// InputRepeatTimer sent it again every 0.1 s for the rest of the session. That
// was the stream of 614-616 the pause menu received with nobody pressing
// anything. So the rewrite is done at GetEngine, the first native InputEvent
// calls, in both builds (the copy-protection check); EU matches after
// IsAnyMoviePlaying, but the earlier point is right for it too.
// That means the same thing a second press of a number key means:
// the first selects the ability and raises targeting, and picking the one
// already selected fires it (DirectPickAbility -> OnAccept).
//
// An ability past the tenth has no number key and so cannot be picked this
// way; the menu says so.
#define INPUT_CMD_ENTER     513
#define INPUT_CMD_ABILITY_1 612
#define ABILITY_KEYS        10
#define PICK_WAIT_MS        500
static int       g_pick_index = -1;     // chosen by the poll, for the hook
static ULONGLONG g_pick_until;
static int       g_pick_release_to = -1; // Enter's release, still to rewrite

// The game's side of the same keys: presses are taken while the menu is open,
// and for a moment after it closes, because the frame that closes the menu may
// run before the game's InputEvent for the key that closed it.
//
// Releases are never taken, because the game already drops them. A press
// swallowed at IsAnyMoviePlaying returns before InputEvent's ActivateTracker,
// so no tracker is made, and PreProcessEventMatching drops the release (EW
// before this hook, EU after it). That is also why the Escape that closed the
// menu cannot reach the game alone.
#define MENU_GRACE_MS 150

static void abar_menu_end(const char* why)
{
    abar_menu_close();
    g_menu_grace_until = GetTickCount64() + MENU_GRACE_MS;
    logf_("abar: menu closed (%s)\n", why);
    speech_cancel_pending();
    speech_say_now(T(TXT_CLOSED));
}

// When the menu last had its keys read. The poll runs from the battle
// cursor's per-frame native, so a mission that ends with the menu open stops
// polling it -- and a menu nobody is polling must not go on taking the arrows
// from whatever screen comes next.
#define MENU_STALE_MS 500
static ULONGLONG g_menu_polled_at;

// Returns 1 while the menu is open, having handled its keys.
int abar_menu_poll(void)
{
    g_menu_polled_at = GetTickCount64();
    int now[MENU_KEYS], pressed[MENU_KEYS];
    for (int k = 0; k < MENU_KEYS; k++) {
        now[k] = k == MENU_KEY_ALT
                     ? input_key_at_down(MENU_ALT_SCAN, "the key two right of P")
                     : (GetAsyncKeyState(g_menu_vk[k]) & 0x8000) != 0;
        pressed[k] = now[k] && !g_menu_was[k];
        g_menu_was[k] = now[k];
    }

    if (!abar_menu_is_open()) {
        if (!(pressed[0] || pressed[MENU_KEY_ALT]) || history_is_open()) return 0;
        // Not under a pause menu, a dialog or a popup: the menu would take
        // the arrows and Enter from the screen the player is actually in.
        if (!hud_has_keys()) {
            logf_("abar: %s with another screen first -- not opened\n",
                  pressed[0] ? "numpad ." : "the key two right of P");
            return 0;
        }
        abar_menu_open();
        logf_("abar: menu opened (%s), %d abilities\n",
              pressed[0] ? "numpad ." : "the key two right of P", abar_count());
        if (abar_count() <= 0) {
            abar_menu_close();
            speech_cancel_pending();
            speech_say_now(T(MENU_NO_ABILITIES));
            return 0;
        }
        abar_say_entry();
        return 1;
    }

    if (pressed[0]) { abar_menu_end("numpad ."); return 0; }
    if (pressed[MENU_KEY_ALT]) { abar_menu_end("the key two right of P"); return 0; }
    if (pressed[6]) { abar_menu_end("Escape"); return 0; }
    if (pressed[7] || pressed[8]) {
        int i = abar_menu_index();
        if (i < 0 || i >= ABILITY_KEYS) {
            logf_("abar: %d has no number key -- cannot be picked\n", i + 1);
            speech_cancel_pending();
            speech_say_now(T(MENU_NO_KEY));
            return 1;
        }
        // Handed to the hook, which rewrites this same keystroke. Silent: the
        // shot readout names the ability as soon as the game selects it.
        g_pick_index = i;
        g_pick_until = GetTickCount64() + PICK_WAIT_MS;
        abar_menu_close();
        g_menu_grace_until = GetTickCount64() + MENU_GRACE_MS;
        logf_("abar: Enter picks ability %d\n", i + 1);
        return 0;
    }
    if (pressed[1] || pressed[4]) { abar_menu_step(-1); abar_say_entry(); }
    else if (pressed[2] || pressed[5]) { abar_menu_step(1); abar_say_entry(); }
    else if (pressed[3]) abar_say_entry();
    return 1;
}

// Whether InputEvent(cmd, mask) belongs to the menu and is to be kept from
// the game. Called from hook_moviecheck.
int abar_menu_swallow(int cmd, int mask)
{
    // Enter is here for an ability with no number key; one that has a key was
    // already rewritten at GetEngine and arrives as that key.
    if (!((cmd >= 500 && cmd <= 503) || cmd == 510 || cmd == INPUT_CMD_ENTER))
        return 0;
    if (!(mask & 1)) return 0;
    ULONGLONG t = GetTickCount64();
    if ((abar_menu_is_open() || history_is_open()) && t - g_menu_polled_at > MENU_STALE_MS) {
        abar_menu_close();
        history_close();
        logf_("abar: menu closed (no longer polled -- the mission ended)\n");
    }
    if (!abar_menu_is_open() && !history_is_open() && t >= g_menu_grace_until) return 0;
    return 1;
}

// Whether the GetEngine hook has anything to do. It runs on every call to
// GetEngine, from everywhere, so this is the whole cost of the hook almost
// all of the time.
int abar_pick_pending(void)
{
    return abar_menu_is_open() || g_pick_release_to >= 0 ||
           (g_pick_index >= 0 && GetTickCount64() < g_pick_until);
}

// Rewrites Enter's InputEvent into the chosen ability's number key. Called
// from GetEngine, whose caller is InputEvent when this does anything at all.
void abar_menu_pick(void* stack)
{
    int mask = 0;
    if (input_event_cmd(stack, &mask) != INPUT_CMD_ENTER) return;
    int32_t* slot = input_event_cmd_slot(stack);
    if (!slot || !writable(slot, sizeof *slot)) return;

    if (mask & 32) {
        if (g_pick_release_to < 0) return;
        *slot = g_pick_release_to;
        logf_("abar: Enter's release -> InputEvent %d, the ability's number key\n",
              g_pick_release_to);
        g_pick_release_to = -1;
        return;
    }
    if (!(mask & 1)) return;

    // The press. The poll may have seen the key first and closed the menu,
    // leaving its choice in g_pick_index; or this may come first, with the
    // menu still open. A press that picks nothing also ends any release still
    // owed, so a release that never came cannot hijack this keystroke's.
    int i = -1;
    if (abar_menu_is_open()) i = abar_menu_index();
    else if (g_pick_index >= 0 && GetTickCount64() < g_pick_until) i = g_pick_index;
    if (i < 0 || i >= ABILITY_KEYS) { g_pick_release_to = -1; return; }

    *slot = INPUT_CMD_ABILITY_1 + i;
    g_pick_release_to = *slot;
    g_pick_index = -1;
    if (abar_menu_is_open()) {
        abar_menu_close();
        g_menu_grace_until = GetTickCount64() + MENU_GRACE_MS;
        logf_("abar: Enter picks ability %d\n", i + 1);
    }
    logf_("abar: Enter -> InputEvent %d\n", *slot);
}

// Insert: the announcements, as a list (history.h). Opens on the newest; Up
// and Down (numpad 8 / 2 or the arrows) walk it, numpad 5 says the entry
// again, Insert or Escape closes it. The arrows and Escape are bound in a
// mission, so while it is open hook_moviecheck keeps them from the game
// through abar_menu_swallow, exactly as for the ability menu. Insert is not:
// its only binding, Camera FreeCam, is removed with -Bindings in
// [Engine.PlayerInput].
#define REVIEW_KEYS 7
static const int g_review_vk[REVIEW_KEYS] = {
    VK_INSERT, VK_NUMPAD8, VK_NUMPAD2, VK_NUMPAD5, VK_UP, VK_DOWN, VK_ESCAPE
};
static int g_review_was[REVIEW_KEYS];

void review_end(const char* why)
{
    history_close();
    g_menu_grace_until = GetTickCount64() + MENU_GRACE_MS;
    logf_("review: closed (%s)\n", why);
    speech_cancel_pending();
    speech_say_now(T(TXT_CLOSED));
}

// Returns 1 while the list is open, having handled its keys.
int review_poll(void)
{
    int now[REVIEW_KEYS], pressed[REVIEW_KEYS];
    for (int k = 0; k < REVIEW_KEYS; k++) {
        now[k] = (GetAsyncKeyState(g_review_vk[k]) & 0x8000) != 0;
        pressed[k] = now[k] && !g_review_was[k];
        g_review_was[k] = now[k];
    }
    // An entry or a page's line, with "Oldest." and the like in front.
    static char say[(HISTORY_TEXT > HISTORY_PAGE_TEXT ? HISTORY_TEXT : HISTORY_PAGE_TEXT) + 64];
    if (!history_is_open()) {
        if (!pressed[0]) return 0;
        g_menu_polled_at = GetTickCount64();
        int opened = history_open(say, sizeof say);
        logf_("review: %s \"%s\"\n", opened ? "opened" : "nothing to open", say);
        speech_cancel_pending();
        speech_say_now(say);
        return opened;
    }
    g_menu_polled_at = GetTickCount64();
    if (pressed[0]) { review_end("Insert"); return 0; }
    if (pressed[6]) { review_end("Escape"); return 0; }
    if (pressed[1] || pressed[4]) history_step(-1, say, sizeof say);
    else if (pressed[2] || pressed[5]) history_step(1, say, sizeof say);
    else if (pressed[3]) history_current(say, sizeof say);
    else return 1;
    speech_cancel_pending();
    speech_say_now(say);
    return 1;
}

int menu_grace(void)
{
    return GetTickCount64() < g_menu_grace_until;
}

void menu_polled(void)
{
    g_menu_polled_at = GetTickCount64();
}

