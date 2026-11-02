// Enum: a tabbed hand menu with movement, a camera view, a target mini-game and visuals.
// Everything here only changes YOUR OWN game. Nothing is sent to other players.
//   Hold Y (left hand)  -> menu on your left hand (B and the right hand with "Right Hand Menu")
//   Touch a button or a tab with the ball on your other index finger to press it
//   Tabs: Move, Cam, Game, Look, Set
// Built from the starter menu's helpers. Rules: AGENTS.md. Lag and crash tips: docs/GUIDE.md.
#include "ymsdk.hpp"
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

using namespace BNM;
using namespace BNM::Structures::Unity;
using BNM::IL2CPP::Il2CppObject;

YEEPS_MOD("Enum")

// ---- small helpers ---------------------------------------------------------

static std::string TypeName(const BNM::IL2CPP::Il2CppType *t) {
    Class c(t);
    if (!c.IsValid() || !c.GetClass() || !c.GetClass()->name) return "?";
    auto *k = c.GetClass();
    return (k->namespaze && k->namespaze[0]) ? std::string(k->namespaze) + "." + k->name : k->name;
}
// Pick an overload by its exact parameter types, e.g. GetComponent(System.Type).
static MethodBase Overload(Class c, const char *name, std::vector<std::string> types) {
    for (auto &m : c.GetMethods(false)) {
        auto *mi = m.GetInfo();
        if (!mi || !mi->name || strcmp(mi->name, name) != 0 || mi->parameters_count != types.size()) continue;
        bool ok = true;
        for (size_t i = 0; i < types.size() && ok; i++) ok = TypeName(mi->parameters[i]) == types[i];
        if (ok) return m;
    }
    return {};
}

// Unity objects we keep between frames are held by a GC handle, or the garbage
// collector can free them under us.
static void *(*p_handleNew)(void *, bool);
static void *(*p_handleTarget)(void *);
static void (*p_handleFree)(void *);
struct Ref {
    void *h = nullptr;
    void set(Il2CppObject *o) { clear(); if (o && p_handleNew) h = p_handleNew(o, false); }
    Il2CppObject *get() const { return h && p_handleTarget ? (Il2CppObject *)p_handleTarget(h) : nullptr; }
    void clear() { if (h && p_handleFree) p_handleFree(h); h = nullptr; }
};

static Color Lerp(Color a, Color b, float t) {
    return Color(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1);
}
static Color Rainbow(float h) {             // hue 0..1 -> bright color
    h = h - floorf(h);
    float r = fabsf(h * 6 - 3) - 1, g = 2 - fabsf(h * 6 - 2), b = 2 - fabsf(h * 6 - 4);
    auto c = [](float v) { return v < 0 ? 0.f : v > 1 ? 1.f : v; };
    return Color(c(r), c(g), c(b), 1);
}
static float Dot(Vector3 a, Vector3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vector3 Cross(Vector3 a, Vector3 b) { return Vector3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static Vector3 Norm(Vector3 v) {
    float l = sqrtf(Dot(v, v));
    return l > 1e-5f ? v * (1.0f / l) : Vector3(0, 0, 1);
}
static Vector3 Flat(Vector3 v) {            // v without its up/down part, length 1
    v.y = 0;
    float l = sqrtf(v.x * v.x + v.z * v.z);
    return l > 1e-4f ? v * (1.0f / l) : Vector3(0, 0, 1);
}
static float Rand01() { static uint32_t x = 2463534242u; x ^= x << 13; x ^= x >> 17; x ^= x << 5; return (x & 0xffff) / 65535.0f; }

// ---- game lookups, resolved once ---------------------------------------------

struct XRDevice { uint64_t id; bool initialized; };
static struct {
    bool ok = false, camOk = false, fogOk = false;
    Method<Il2CppObject *> createPrimitive, goTransform, goGetComponent, goAddComponent, compTransform, compGameObject,
                           rendererGetMaterial, shaderFind, builtinResource, findAll, rtGet;
    Method<void> goSetActive, destroy, trSetParent, trSetPosition, trSetRotation, trSetLocalPosition, trSetLocalRotation,
                 trSetLocalScale, matSetColor, matSetShader, matSetTexture, textSetFont, textSetText, textSetFontSize,
                 textSetAlignment, graphicSetColor, canvasSetRenderMode, rtSetSizeDelta, textSetBestFit, textSetMinSize,
                 textSetMaxSize, textSetFontStyle, teleportTo, behaviourSetEnabled, goSetLayer, plSetVelocity,
                 plNoGravityFor, rtRelease, camSetTarget, camSetFov, camSetCull, fogSet, fogColorSet, fogDensitySet;
    Method<Quaternion> trGetRotation;
    Method<Vector3> trGetPosition, trGetForward, trGetRight, trGetUp, trInverseTransformPoint, plGetVelocity;
    Method<bool> alive, tryGetBool, haptic, fogGet;
    Method<Color> fogColorGet;
    Method<float> fogDensityGet;
    Method<XRDevice> deviceAtNode;
    Field<Il2CppObject *> leftHand, rightHand, headTransform, bodyCollider;
    Field<float> maxJumpSpeed, jumpMultiplier;
    Field<int> locomotionLayers;
    BNM::MonoType *tCollider = nullptr, *tCanvas = nullptr, *tText = nullptr, *tFont = nullptr, *tRenderer = nullptr,
                  *tCamera = nullptr;
    Ref shader, font, usagePrimary, usageSecondary, usageGrip, usageTrigger;
} G;

static bool Lookups() {
    if (G.ok) return true;
    auto R = ym::api()->resolve;
    p_handleNew = (decltype(p_handleNew))R("il2cpp_gchandle_new");
    p_handleTarget = (decltype(p_handleTarget))R("il2cpp_gchandle_get_target");
    p_handleFree = (decltype(p_handleFree))R("il2cpp_gchandle_free");
    Class go("UnityEngine", "GameObject"), tr("UnityEngine", "Transform"), obj("UnityEngine", "Object"),
          mat("UnityEngine", "Material"), rend("UnityEngine", "Renderer"), sh("UnityEngine", "Shader"),
          res("UnityEngine", "Resources"), text("UnityEngine.UI", "Text"), graphic("UnityEngine.UI", "Graphic"),
          canvas("UnityEngine", "Canvas"), rt("UnityEngine", "RectTransform"), dev("UnityEngine.XR", "InputDevices"),
          player("", "MasterPlayer", Image("Assembly-CSharp.dll"));
    if (!go || !tr || !player || !dev) { ym::log("my-mod: game classes missing"); return false; }
    G.createPrimitive = go.GetMethod("CreatePrimitive", 1);
    G.goTransform = go.GetMethod("get_transform", 0);
    G.goGetComponent = Overload(go, "GetComponent", {"System.Type"});
    G.goAddComponent = Overload(go, "AddComponent", {"System.Type"});
    G.goSetActive = go.GetMethod("SetActive", 1);
    G.goSetLayer = go.GetMethod("set_layer", 1);
    Class comp("UnityEngine", "Component");
    G.compTransform = comp.GetMethod("get_transform", 0);
    G.compGameObject = comp.GetMethod("get_gameObject", 0);
    G.destroy = Overload(obj, "Destroy", {"UnityEngine.Object"});
    G.alive = obj.GetMethod("op_Implicit", 1);
    G.trSetParent = Overload(tr, "SetParent", {"UnityEngine.Transform", "System.Boolean"});
    G.trSetPosition = tr.GetMethod("set_position", 1);
    G.trSetRotation = tr.GetMethod("set_rotation", 1);
    G.trSetLocalPosition = tr.GetMethod("set_localPosition", 1);
    G.trSetLocalRotation = tr.GetMethod("set_localRotation", 1);
    G.trSetLocalScale = tr.GetMethod("set_localScale", 1);
    G.trGetPosition = tr.GetMethod("get_position", 0);
    G.trGetRotation = tr.GetMethod("get_rotation", 0);
    G.trGetForward = tr.GetMethod("get_forward", 0);
    G.trGetRight = tr.GetMethod("get_right", 0);
    G.trGetUp = tr.GetMethod("get_up", 0);
    G.trInverseTransformPoint = Overload(tr, "InverseTransformPoint", {"UnityEngine.Vector3"});
    G.rendererGetMaterial = rend.GetMethod("get_material", 0);
    G.matSetColor = Overload(mat, "set_color", {"UnityEngine.Color"});
    G.matSetShader = mat.GetMethod("set_shader", 1);
    G.matSetTexture = mat.GetMethod("set_mainTexture", 1);
    G.shaderFind = sh.GetMethod("Find", 1);
    G.findAll = Overload(res, "FindObjectsOfTypeAll", {"System.Type"});
    G.builtinResource = Overload(res, "GetBuiltinResource", {"System.Type", "System.String"});
    G.textSetFont = text.GetMethod("set_font", 1);
    G.textSetFontStyle = text.GetMethod("set_fontStyle", 1);
    G.textSetBestFit = text.GetMethod("set_resizeTextForBestFit", 1);
    G.textSetMinSize = text.GetMethod("set_resizeTextMinSize", 1);
    G.textSetMaxSize = text.GetMethod("set_resizeTextMaxSize", 1);
    G.textSetText = text.GetMethod("set_text", 1);
    G.textSetFontSize = text.GetMethod("set_fontSize", 1);
    G.textSetAlignment = text.GetMethod("set_alignment", 1);
    G.graphicSetColor = graphic.GetMethod("set_color", 1);
    G.canvasSetRenderMode = canvas.GetMethod("set_renderMode", 1);
    G.rtSetSizeDelta = rt.GetMethod("set_sizeDelta", 1);
    G.deviceAtNode = dev.GetMethod("GetDeviceAtXRNode", 1);
    G.tryGetBool = dev.GetMethod("TryGetFeatureValue_bool", 3);
    G.haptic = dev.GetMethod("SendHapticImpulse", 4);       // optional: a small buzz on press
    G.leftHand = player.GetField("leftHandTransform");
    G.rightHand = player.GetField("rightHandTransform");
    G.headTransform = player.GetField("headTransform");
    G.bodyCollider = player.GetField("bodyCollider");
    G.maxJumpSpeed = player.GetField("maxJumpSpeed");
    G.jumpMultiplier = player.GetField("jumpMultiplier");
    G.locomotionLayers = player.GetField("locomotionEnabledLayers");
    // Yeeps moves your body through these, not through a Rigidbody on the player object
    G.plGetVelocity = player.GetMethod("GetVelocity", 0);
    G.plSetVelocity = player.GetMethod("ApplySetVelocityNoHaptic", 1);
    if (!G.plSetVelocity.IsValid()) G.plSetVelocity = player.GetMethod("ApplySetVelocity", 1);
    G.plNoGravityFor = player.GetMethod("ApplyNoGravityDuration", 1);
    G.teleportTo = player.GetMethod("TeleportTo", 1);
    G.behaviourSetEnabled = Class("UnityEngine", "Behaviour").GetMethod("set_enabled", 1);
    G.tCollider = Class("UnityEngine", "Collider").GetMonoType();
    G.tRenderer = rend.GetMonoType();
    G.tCanvas = canvas.GetMonoType();
    G.tText = text.GetMonoType();
    G.tFont = Class("UnityEngine", "Font").GetMonoType();

    // Camera view (optional: if the game build stripped any of this, the Cam tab says so)
    Class cam("UnityEngine", "Camera"), rtex("UnityEngine", "RenderTexture");
    if (cam.IsValid() && rtex.IsValid()) {
        G.tCamera = cam.GetMonoType();
        G.camSetTarget = cam.GetMethod("set_targetTexture", 1);
        G.camSetFov = cam.GetMethod("set_fieldOfView", 1);
        G.camSetCull = cam.GetMethod("set_cullingMask", 1);
        G.rtGet = Overload(rtex, "GetTemporary", {"System.Int32", "System.Int32", "System.Int32"});
        G.rtRelease = Overload(rtex, "ReleaseTemporary", {"UnityEngine.RenderTexture"});
    }
    G.camOk = G.tCamera && G.camSetTarget.IsValid() && G.camSetFov.IsValid() && G.camSetCull.IsValid() &&
              G.rtGet.IsValid() && G.rtRelease.IsValid() && G.matSetTexture.IsValid() && G.behaviourSetEnabled.IsValid();
    // Fog (optional)
    Class rs("UnityEngine", "RenderSettings");
    if (rs.IsValid()) {
        G.fogSet = rs.GetMethod("set_fog", 1);
        G.fogGet = rs.GetMethod("get_fog", 0);
        G.fogColorSet = rs.GetMethod("set_fogColor", 1);
        G.fogColorGet = rs.GetMethod("get_fogColor", 0);
        G.fogDensitySet = rs.GetMethod("set_fogDensity", 1);
        G.fogDensityGet = rs.GetMethod("get_fogDensity", 0);
    }
    G.fogOk = G.fogSet.IsValid() && G.fogGet.IsValid() && G.fogColorSet.IsValid() && G.fogColorGet.IsValid() &&
              G.fogDensitySet.IsValid() && G.fogDensityGet.IsValid();
    ym::log("my-mod: camera %d, fog %d, velocity %d/%d", G.camOk, G.fogOk, G.plGetVelocity.IsValid(), G.plSetVelocity.IsValid());

    G.usagePrimary.set((Il2CppObject *)CreateMonoString("PrimaryButton"));
    G.usageSecondary.set((Il2CppObject *)CreateMonoString("SecondaryButton"));
    G.usageGrip.set((Il2CppObject *)CreateMonoString("GripButton"));
    G.usageTrigger.set((Il2CppObject *)CreateMonoString("TriggerButton"));
    for (const char *s : {"Universal Render Pipeline/Unlit", "Universal Render Pipeline/Lit"})
        if (!G.shader.get() && G.shaderFind.IsValid()) G.shader.set(G.shaderFind((BNM::Structures::Mono::String *)CreateMonoString(s)));
    for (const char *f : {"LegacyRuntime.ttf", "Arial.ttf"})
        if (!G.font.get() && G.builtinResource.IsValid() && G.tFont)
            G.font.set(G.builtinResource((Il2CppObject *)G.tFont, CreateMonoString(f)));
    if (!G.font.get() && G.findAll.IsValid() && G.tFont) {   // else any font the game loaded (one time)
        auto *arr = (uint8_t *)G.findAll((Il2CppObject *)G.tFont);
        if (arr && *(uintptr_t *)(arr + 0x18) > 0) G.font.set(((Il2CppObject **)(arr + 0x20))[0]);
    }
    G.ok = G.createPrimitive.IsValid() && G.goTransform.IsValid() && G.trSetPosition.IsValid() &&
           G.deviceAtNode.IsValid() && G.tryGetBool.IsValid() && G.leftHand.IsValid();
    if (!G.ok) ym::log("my-mod: some game methods are missing, menu off");
    return G.ok;
}

static bool Alive(Il2CppObject *o) { return o && G.alive((Il2CppObject *)o); }
static Il2CppObject *Tr(Il2CppObject *gameObject) { return G.goTransform[gameObject](); }
static void Show(Ref &r, bool on) { if (Alive(r.get())) G.goSetActive[r.get()](on); }
static void Kill(Ref &r) { if (Alive(r.get())) G.destroy(r.get()); r.clear(); }
static Il2CppObject *NewObject(const char *name) {
    return Class("UnityEngine", "GameObject").CreateNewObjectParameters(CreateMonoString(name));
}

// A primitive with a flat color. collider=false removes its collider so it can't push you.
// Always give new objects the URP shader: Unity's default material only draws in one eye on Quest.
static Il2CppObject *Prim(int type, Color c, bool collider) {
    Il2CppObject *o = G.createPrimitive(type);   // 0 = Sphere, 3 = Cube, 5 = Quad
    if (!o) return nullptr;
    if (!collider) if (auto *col = G.goGetComponent[o](G.tCollider)) G.destroy(col);
    if (auto *r = G.goGetComponent[o](G.tRenderer)) {
        Il2CppObject *m = G.rendererGetMaterial[r]();
        if (m && G.shader.get()) G.matSetShader[m](G.shader.get());
        if (m) G.matSetColor[m](c);
    }
    return o;
}
static Il2CppObject *Cube(Color c, bool collider) { return Prim(3, c, collider); }
static void SetColor(Il2CppObject *o, Color c) {
    if (!Alive(o)) return;
    if (auto *r = G.goGetComponent[o](G.tRenderer)) if (Il2CppObject *m = G.rendererGetMaterial[r]()) G.matSetColor[m](c);
}
static void SetText(Ref &text, const char *s) {
    if (Alive(text.get())) G.textSetText[text.get()](CreateMonoString(s));
}

// ---- input -------------------------------------------------------------------

struct Buttons { bool a = false, b = false, x = false, y = false, lgrip = false, rgrip = false, ltrig = false, rtrig = false; };
static bool Pressed(int node, Ref &usage) {
    XRDevice d = G.deviceAtNode(node);
    bool v = false;
    if (d.initialized) G.tryGetBool(d.id, (BNM::Structures::Mono::String *)usage.get(), &v);
    return v;
}
static Buttons ReadButtons() {
    const int left = 4, right = 5;              // XRNode.LeftHand / RightHand
    Buttons b;
    b.x = Pressed(left, G.usagePrimary);  b.y = Pressed(left, G.usageSecondary);
    b.a = Pressed(right, G.usagePrimary); b.b = Pressed(right, G.usageSecondary);
    b.lgrip = Pressed(left, G.usageGrip); b.rgrip = Pressed(right, G.usageGrip);
    b.ltrig = Pressed(left, G.usageTrigger); b.rtrig = Pressed(right, G.usageTrigger);
    return b;
}
static void Buzz(int node) {
    if (!G.haptic.IsValid()) return;
    XRDevice d = G.deviceAtNode(node);
    if (d.initialized) G.haptic(d.id, 0u, 0.35f, 0.04f);
}

// ---- state --------------------------------------------------------------------

// Feature switches. Every feature only changes your own game.
static bool f_fly, f_fastFly, f_platforms, f_speedBoost, f_lowGravity, f_noGravity;     // Move
static bool f_cam;                                                                      // Cam
static bool f_game;                                                                     // Game
static bool f_fog, f_fps, f_clock;                                                      // Look
static bool f_rainbow, f_rightHand, f_notify = true;                                    // Set

static Ref g_player, g_leftHand, g_rightHand, g_head;
static Vector3 g_spot;
static bool g_haveSpot = false;
static double g_time = 0;

static void Notify(const char *fmt, ...);

// ---- menu entries ----------------------------------------------------------------

enum Cat { C_MOVE, C_CAM, C_GAME, C_LOOK, C_SET, kTabs };       // the categories on the Home page
static const char *kCatName[kTabs] = {"Movement", "Camera", "Target Game", "Visual", "Settings"};
static const char *kCatTip[kTabs] = {"Fly, platforms, jumps and teleport spots.", "A camera view of yourself on a floating screen.",
                                     "A target game you play with your right hand.", "Fog and on-screen info.", "Menu look and feel."};

enum Kind { TOGGLE, ACTION };
struct Entry {
    Cat cat;
    const char *label;
    Kind kind;
    bool *flag;                    // TOGGLE: the switch
    void (*fn)();                  // ACTION, or called after a TOGGLE changes
    const char *(*detail)();       // optional: the current value, e.g. "Blue"
    const char *tip;
    bool fav = false;              // starred: also listed on the Favorites page (kept while the game is open)
};

static void SaveSpot();
static void GoToSpot();
static void ApplySpeedBoost();
static void ApplyGravity();
static void ApplyCam();
static void NextCamView();
static void NextCamDist();
static void NextCamZoom();
static void NextCamSide();
static void NextCamSize();
static const char *CamViewName();
static const char *CamDistName();
static const char *CamZoomName();
static const char *CamSideName();
static const char *CamSizeName();
static void ApplyGame();
static void NextDifficulty();
static void ResetBest();
static const char *DifficultyName();
static const char *BestName();
static void ApplyFog();
static void NextFogColor();
static void NextFogThickness();
static const char *FogColorName();
static const char *FogThicknessName();
static void NextTheme();
static const char *ThemeName();
static void AllOff();

static Entry g_entries[] = {
    {C_MOVE, "Fly", TOGGLE, &f_fly, nullptr, nullptr, "Hold A to fly where you look."},
    {C_MOVE, "Fast Fly", TOGGLE, &f_fastFly, nullptr, nullptr, "Makes Fly a lot faster."},
    {C_MOVE, "Platforms", TOGGLE, &f_platforms, nullptr, nullptr, "Hold a grip to stand on a platform under that hand."},
    {C_MOVE, "Speed Boost", TOGGLE, &f_speedBoost, ApplySpeedBoost, nullptr, "Faster and higher jumps."},
    {C_MOVE, "Low Gravity", TOGGLE, &f_lowGravity, nullptr, nullptr, "Jump higher, fall slower."},
    {C_MOVE, "No Gravity", TOGGLE, &f_noGravity, ApplyGravity, nullptr, "Turns your gravity off."},
    {C_MOVE, "Save Spot", ACTION, nullptr, SaveSpot, nullptr, "Remembers where you are standing."},
    {C_MOVE, "Go To Spot", ACTION, nullptr, GoToSpot, nullptr, "Teleports you to your saved spot."},

    {C_CAM, "Camera", TOGGLE, &f_cam, ApplyCam, nullptr, "A small screen in front of you shows a camera view of you."},
    {C_CAM, "View", ACTION, nullptr, NextCamView, CamViewName, "Selfie looks at you from the front. Behind is over your shoulder."},
    {C_CAM, "Distance", ACTION, nullptr, NextCamDist, CamDistName, "How far the camera is from you."},
    {C_CAM, "Zoom", ACTION, nullptr, NextCamZoom, CamZoomName, "Wide, normal or zoomed in."},
    {C_CAM, "Screen Side", ACTION, nullptr, NextCamSide, CamSideName, "Where the screen floats in front of you."},
    {C_CAM, "Screen Size", ACTION, nullptr, NextCamSize, CamSizeName, "Small, medium or large screen."},

    {C_GAME, "Target Practice", TOGGLE, &f_game, ApplyGame, nullptr, "Touch the balls with your right hand before time runs out."},
    {C_GAME, "Difficulty", ACTION, nullptr, NextDifficulty, DifficultyName, "Easy has big targets and more time."},
    {C_GAME, "Best Score", ACTION, nullptr, ResetBest, BestName, "Press to reset your best score."},

    {C_LOOK, "Fog", TOGGLE, &f_fog, ApplyFog, nullptr, "Thick coloured fog, only for you."},
    {C_LOOK, "Fog Colour", ACTION, nullptr, NextFogColor, FogColorName, "Changes the fog colour."},
    {C_LOOK, "Fog Thickness", ACTION, nullptr, NextFogThickness, FogThicknessName, "Light, medium or heavy fog."},
    {C_LOOK, "Show FPS", TOGGLE, &f_fps, nullptr, nullptr, "Frames per second at the top of your view."},
    {C_LOOK, "Show Clock", TOGGLE, &f_clock, nullptr, nullptr, "The time at the top of your view."},

    {C_SET, "Theme", ACTION, nullptr, NextTheme, ThemeName, "Changes the menu colours."},
    {C_SET, "Rainbow Border", TOGGLE, &f_rainbow, nullptr, nullptr, "The menu border cycles through colours."},
    {C_SET, "Right Hand Menu", TOGGLE, &f_rightHand, nullptr, nullptr, "Hold B, menu on your right hand."},
    {C_SET, "Notifications", TOGGLE, &f_notify, nullptr, nullptr, "Messages when you switch something."},
};
static constexpr int kEntries = sizeof(g_entries) / sizeof(g_entries[0]);

static_assert(kEntries <= 32, "raise the size of g_fav");
static bool g_fav[32];                        // starred entries (kept while the game is open)

// ---- themes ---------------------------------------------------------------------
// bgA/bgB: the dark board (it breathes between them). button: the cards. buttonOn: the
// selected tab and switched-on rows. title: the outline and the small text.

struct Theme { const char *name; Color bgA, bgB, button, buttonOn, title; };
static const Theme kThemes[] = {
    {"Violet", Color(0.09f, 0.05f, 0.17f, 1), Color(0.14f, 0.07f, 0.25f, 1), Color(0.20f, 0.12f, 0.32f, 1), Color(0.52f, 0.30f, 0.88f, 1), Color(0.72f, 0.58f, 1.0f, 1)},
    {"Blaze",  Color(0.16f, 0.04f, 0.03f, 1), Color(0.26f, 0.07f, 0.03f, 1), Color(0.34f, 0.09f, 0.05f, 1), Color(1.0f, 0.45f, 0.05f, 1),  Color(1.0f, 0.62f, 0.20f, 1)},
    {"Blue",   Color(0.04f, 0.07f, 0.16f, 1), Color(0.06f, 0.11f, 0.25f, 1), Color(0.10f, 0.17f, 0.35f, 1), Color(0.20f, 0.50f, 1.0f, 1),  Color(0.70f, 0.85f, 1.0f, 1)},
    {"Green",  Color(0.03f, 0.11f, 0.05f, 1), Color(0.05f, 0.17f, 0.07f, 1), Color(0.09f, 0.26f, 0.12f, 1), Color(0.25f, 0.80f, 0.30f, 1), Color(0.75f, 1.0f, 0.75f, 1)},
    {"Dark",   Color(0.07f, 0.07f, 0.08f, 1), Color(0.11f, 0.11f, 0.13f, 1), Color(0.18f, 0.18f, 0.21f, 1), Color(0.55f, 0.55f, 0.62f, 1), Color(0.85f, 0.85f, 0.90f, 1)},
};
static constexpr int kThemeCount = sizeof(kThemes) / sizeof(kThemes[0]);
static int g_theme = 0;
static const Theme &T() { return kThemes[g_theme]; }
static const char *ThemeName() { return T().name; }
static void NextTheme() { g_theme = (g_theme + 1) % kThemeCount; }

// ---- menu objects -----------------------------------------------------------------
// The board: +x goes across, +y goes up, you look at it from -z. Meters.
// Top to bottom: header card (title + Reset), Home / Favorites tabs, an info bar that shows
// the tip of the button under your finger, the page count, 5 list rows (title, small
// subtitle, arrow or star), and a footer with previous / next.

static constexpr int kRows = 5;
static const float kBoardW = 0.31f, kBoardH = 0.52f, kRowW = 0.27f, kRowH = 0.05f;
static const float kHeaderY = 0.212f, kTabY = 0.153f, kInfoY = 0.107f, kCountY = 0.074f, kFootY = -0.226f;
static const float kTabW = 0.132f, kTabH = 0.034f, kTabX = 0.068f, kResetX = 0.087f, kResetW = 0.07f, kResetH = 0.032f;
static const float kNavW = 0.1f, kNavH = 0.04f, kNavX = 0.085f, kStarX = 0.1f, kStarW = 0.045f;
static const float kMenuScale = 0.8f;                      // the whole menu is a bit smaller than its board
static float RowY(int r) { return 0.035f - r * 0.058f; }
static const Color kWhite(1, 1, 1, 1), kGold(1.0f, 0.80f, 0.20f, 1), kResetRed(0.45f, 0.16f, 0.24f, 1);

enum Hit { H_NONE = -2, H_PREV = -1, H_NEXT = 50, H_RESET = 200 };      // rows are 0..4, stars 10..14, tabs 100..101

static Ref g_menu, g_board, g_bg, g_outline, g_pointer, g_title, g_titleBar, g_tipText, g_countText, g_navText,
           g_resetCube, g_infoBar, g_prev, g_next;
static Ref g_tabCube[2], g_rowCube[kRows], g_rowGo[kRows][3], g_rowTx[kRows][3];
static Ref g_notifyGo, g_notifyText, g_hudGo, g_hudText, g_leftPlatform, g_rightPlatform;
static double g_openTime = 0, g_flashUntil = 0, g_nextPress = 0, g_notifyUntil = 0, g_nextTitle = 0, g_nextHud = 0;
static int g_flashRow = -2, g_hover = -2, g_touching = -2, g_frames = 0;
static bool g_built = false, g_menuShown = false, g_dirty = true;
static int g_home = 0;                  // 0 = Home tab, 1 = Favorites tab
static int g_cat = -1;                  // -1 = the category list, else the open category
static int g_page = 0;
static float g_fps = 0;

// What one row shows: an entry, a category (cat >= 0) or the "Back" row (e == null, cat < 0).
struct Row { Entry *e; int cat; };
static std::vector<Row> g_view;
static int EntryIndex(Entry *e) { return (int)(e - g_entries); }
static int PageCount() { return g_view.empty() ? 1 : (int)(g_view.size() + kRows - 1) / kRows; }

static void BuildView() {
    g_view.clear();
    if (g_home == 1) {
        for (int i = 0; i < kEntries; i++) if (g_fav[i]) g_view.push_back({&g_entries[i], -1});
    } else if (g_cat < 0) {
        for (int c = 0; c < kTabs; c++) g_view.push_back({nullptr, c});
    } else {
        g_view.push_back({nullptr, -1});
        for (auto &e : g_entries) if (e.cat == (Cat)g_cat) g_view.push_back({&e, -1});
    }
    if (g_page >= PageCount()) g_page = 0;
}

static Il2CppObject *Label(Il2CppObject *canvasT, const char *s, Vector3 pos, float w, float h, int size, Ref *textOut, int align = 4) {
    if (!G.font.get() || !G.tText) return nullptr;
    Il2CppObject *go = NewObject("Label");
    Il2CppObject *text = go ? G.goAddComponent[go](G.tText) : nullptr;
    if (!text) return nullptr;
    Il2CppObject *t = Tr(go);
    G.trSetParent[t](canvasT, false);
    G.rtSetSizeDelta[t](Vector2(w * 1000, h * 1000));
    G.trSetLocalPosition[t](pos * 1000);
    G.trSetLocalRotation[t](Quaternion(0, 0, 0, 1));
    G.trSetLocalScale[t](Vector3(1, 1, 1));
    G.textSetFont[text](G.font.get());
    G.textSetText[text](CreateMonoString(s));
    G.textSetFontSize[text](size);
    G.textSetAlignment[text](align);             // 4 = MiddleCenter, 3 = MiddleLeft, 5 = MiddleRight
    if (G.textSetFontStyle.IsValid()) G.textSetFontStyle[text](1);     // Bold
    if (G.textSetBestFit.IsValid()) {            // shrink long labels to fit
        G.textSetBestFit[text](true);
        G.textSetMinSize[text](1);
        G.textSetMaxSize[text](size);
    }
    G.graphicSetColor[text](kWhite);
    if (textOut) textOut->set(text);
    return go;
}

// A world-space canvas (1 unit = 1 mm) under parent.
static Il2CppObject *Canvas(Il2CppObject *parentT, Vector3 pos, float w, float h) {
    Il2CppObject *go = NewObject("MyModText");
    Il2CppObject *canvas = go && G.tCanvas ? G.goAddComponent[go](G.tCanvas) : nullptr;
    if (!canvas) return nullptr;
    G.canvasSetRenderMode[canvas](2);            // WorldSpace
    Il2CppObject *ct = Tr(go);
    G.trSetParent[ct](parentT, false);
    G.rtSetSizeDelta[ct](Vector2(w * 1000, h * 1000));
    G.trSetLocalPosition[ct](pos);
    G.trSetLocalRotation[ct](Quaternion(0, 0, 0, 1));
    G.trSetLocalScale[ct](Vector3(0.001f, 0.001f, 0.001f));
    return go;
}

static Il2CppObject *Part(Il2CppObject *parentT, Color c, Vector3 pos, Vector3 scale) {
    Il2CppObject *o = Cube(c, false);
    if (!o) return nullptr;
    Il2CppObject *t = Tr(o);
    G.trSetParent[t](parentT, false);
    G.trSetLocalPosition[t](pos);
    G.trSetLocalScale[t](scale);
    return o;
}

static void PointerToHand() {
    Il2CppObject *p = g_pointer.get(), *hand = f_rightHand ? g_leftHand.get() : g_rightHand.get();
    if (!Alive(p) || !hand) return;
    Il2CppObject *t = Tr(p);
    Vector3 at(0.01f, -0.017f, 0.19f);           // the tip of your index finger
    if (f_rightHand) at.x = -at.x;
    G.trSetParent[t](hand, false);
    G.trSetLocalPosition[t](at);
}

// Built once per room; afterwards only shown/hidden, moved and relabeled.
static void BuildMenu() {
    Il2CppObject *menu = NewObject("MyModMenu"), *board = NewObject("MyModBoard");
    if (!menu || !board) return;
    g_menu.set(menu);
    g_board.set(board);
    Il2CppObject *bt = Tr(board);
    G.trSetParent[bt](Tr(menu), false);
    G.trSetLocalPosition[bt](Vector3(0.1f, 0, 0));
    G.trSetLocalRotation[bt](Quaternion::LookRotation(Vector3(-1, 0, 0), Vector3(0, 0, 1)));

    g_outline.set(Part(bt, T().title, Vector3(0, 0, 0.004f), Vector3(kBoardW + 0.008f, kBoardH + 0.008f, 0.004f)));
    g_bg.set(Part(bt, T().bgA, Vector3(0, 0, 0), Vector3(kBoardW, kBoardH, 0.01f)));
    g_titleBar.set(Part(bt, T().button, Vector3(0, kHeaderY, -0.002f), Vector3(kRowW, 0.06f, 0.009f)));
    g_resetCube.set(Part(bt, kResetRed, Vector3(kResetX, kHeaderY, -0.006f), Vector3(kResetW, kResetH, 0.006f)));
    for (int t = 0; t < 2; t++)
        g_tabCube[t].set(Part(bt, T().button, Vector3(t ? kTabX : -kTabX, kTabY, -0.005f), Vector3(kTabW, kTabH, 0.009f)));
    g_infoBar.set(Part(bt, T().button, Vector3(0, kInfoY, -0.005f), Vector3(kRowW, 0.034f, 0.009f)));
    for (int r = 0; r < kRows; r++)
        g_rowCube[r].set(Part(bt, T().button, Vector3(0, RowY(r), -0.005f), Vector3(kRowW, kRowH, 0.009f)));
    g_prev.set(Part(bt, T().button, Vector3(-kNavX, kFootY, -0.005f), Vector3(kNavW, kNavH, 0.009f)));
    g_next.set(Part(bt, T().button, Vector3(kNavX, kFootY, -0.005f), Vector3(kNavW, kNavH, 0.009f)));

    if (Il2CppObject *canvas = Canvas(bt, Vector3(0, 0, -0.0105f), kBoardW + 0.1f, kBoardH + 0.1f)) {
        Il2CppObject *ct = Tr(canvas);
        Label(ct, "Enum", Vector3(-0.025f, kHeaderY, 0), 0.19f, 0.05f, 30, &g_title, 3);
        Label(ct, "Reset", Vector3(kResetX, kHeaderY, 0), kResetW, kResetH, 14, nullptr);
        Label(ct, "HOME", Vector3(-kTabX, kTabY, 0), kTabW, kTabH, 14, nullptr);
        Label(ct, "FAVORITES", Vector3(kTabX, kTabY, 0), kTabW, kTabH, 14, nullptr);
        Label(ct, "", Vector3(0.0f, kInfoY, 0), kRowW - 0.03f, 0.034f, 13, &g_tipText, 3);
        Label(ct, "01 / 01", Vector3(0.095f, kCountY, 0), 0.08f, 0.02f, 11, &g_countText, 5);
        for (int r = 0; r < kRows; r++) {
            g_rowGo[r][0].set(Label(ct, "", Vector3(-0.03f, RowY(r) + 0.011f, 0), 0.2f, 0.026f, 18, &g_rowTx[r][0], 3));
            g_rowGo[r][1].set(Label(ct, "", Vector3(-0.03f, RowY(r) - 0.012f, 0), 0.2f, 0.02f, 12, &g_rowTx[r][1], 3));
            g_rowGo[r][2].set(Label(ct, "", Vector3(kStarX, RowY(r), 0), kStarW, 0.045f, 26, &g_rowTx[r][2], 4));
        }
        Label(ct, "<", Vector3(-kNavX, kFootY, 0), kNavW, kNavH, 30, nullptr);
        Label(ct, ">", Vector3(kNavX, kFootY, 0), kNavW, kNavH, 30, nullptr);
        Label(ct, "01 / 01", Vector3(0, kFootY, 0), 0.07f, 0.03f, 14, &g_navText);
    }
    // Pointer: a small ball on the index finger of the other hand
    if (Il2CppObject *p = Prim(0, T().buttonOn, false)) {
        G.trSetLocalScale[Tr(p)](Vector3(0.01f, 0.01f, 0.01f));
        g_pointer.set(p);
        PointerToHand();
    }
    // Message line low in your view, and an info line (fps, clock) at the top
    if (Il2CppObject *n = Canvas(g_head.get(), Vector3(0, -0.12f, 0.6f), 0.5f, 0.05f)) {
        g_notifyGo.set(n);
        Label(Tr(n), "", Vector3(0, 0, 0), 0.5f, 0.05f, 22, &g_notifyText);
        G.goSetActive[n](false);
    }
    if (Il2CppObject *h = Canvas(g_head.get(), Vector3(0, 0.13f, 0.6f), 0.4f, 0.04f)) {
        g_hudGo.set(h);
        Label(Tr(h), "", Vector3(0, 0, 0), 0.4f, 0.04f, 20, &g_hudText);
        G.goSetActive[h](false);
    }
    G.goSetActive[menu](false);
    Show(g_pointer, false);
    g_built = true;
    g_dirty = true;
}

static void Notify(const char *fmt, ...) {
    if (!f_notify || !Alive(g_notifyGo.get())) return;
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    SetText(g_notifyText, buf);
    G.goSetActive[g_notifyGo.get()](true);
    g_notifyUntil = g_time + 2.0;
}
// The game's live score line: shown even when Notifications are off.
static void GameText(const char *s) {
    if (!Alive(g_notifyGo.get())) return;
    SetText(g_notifyText, s);
    G.goSetActive[g_notifyGo.get()](true);
    g_notifyUntil = g_time + 0.8;
}

// Relabels and recolors everything. Only runs after something changed.
static void RenderPage() {
    BuildView();
    const Color card = T().button, panel = Lerp(T().button, T().bgA, 0.45f);
    for (int r = 0; r < kRows; r++) {
        int i = g_page * kRows + r;
        bool used = i < (int)g_view.size();
        Show(g_rowCube[r], used);
        for (int k = 0; k < 3; k++) Show(g_rowGo[r][k], used);
        if (!used) continue;
        Row &w = g_view[i];
        char title[48], sub[48];
        const char *mark = ">";
        bool on = false;
        Color markColor = T().title;
        if (w.e) {
            Entry *e = w.e;
            on = e->kind == TOGGLE && *e->flag;
            snprintf(title, sizeof title, "%s", e->label);
            if (e->detail) snprintf(sub, sizeof sub, "%s", e->detail());
            else snprintf(sub, sizeof sub, "%s", e->kind == TOGGLE ? (on ? "On" : "Off") : "Press");
            bool fav = g_fav[EntryIndex(e)];
            mark = "*";
            markColor = fav ? kGold : Color(0.45f, 0.40f, 0.55f, 1);
        } else if (w.cat >= 0) {
            snprintf(title, sizeof title, "%s", kCatName[w.cat]);
            snprintf(sub, sizeof sub, "Open / select");
        } else {
            snprintf(title, sizeof title, "Back");
            snprintf(sub, sizeof sub, "All categories");
            mark = "<";
        }
        SetText(g_rowTx[r][0], title);
        SetText(g_rowTx[r][1], sub);
        SetText(g_rowTx[r][2], mark);
        bool flash = r == g_flashRow;
        SetColor(g_rowCube[r].get(), flash ? Lerp(T().buttonOn, kWhite, 0.4f) : on ? T().buttonOn : card);
        if (Il2CppObject *rc = g_rowCube[r].get())            // a pressed row dips a little
            G.trSetLocalScale[Tr(rc)](flash ? Vector3(kRowW * 0.97f, kRowH * 0.9f, 0.009f) : Vector3(kRowW, kRowH, 0.009f));
        if (Alive(g_rowTx[r][1].get())) G.graphicSetColor[g_rowTx[r][1].get()](on ? kWhite : T().title);
        if (Alive(g_rowTx[r][2].get())) G.graphicSetColor[g_rowTx[r][2].get()](markColor);
    }
    char count[24];
    snprintf(count, sizeof count, "%02d / %02d", g_page + 1, PageCount());
    SetText(g_countText, count);
    SetText(g_navText, count);
    bool pages = PageCount() > 1;
    Show(g_prev, pages);
    Show(g_next, pages);
    SetColor(g_prev.get(), card);
    SetColor(g_next.get(), card);
    for (int t = 0; t < 2; t++) SetColor(g_tabCube[t].get(), t == g_home ? T().buttonOn : panel);
    SetColor(g_titleBar.get(), panel);
    SetColor(g_infoBar.get(), panel);
    SetColor(g_pointer.get(), T().buttonOn);
    g_nextTitle = 0;                     // refresh the title now
    g_dirty = false;
}

// The title is "Enum", with the open category and the fps beside it. Five times a second.
static void UpdateTitle() {
    if (g_time < g_nextTitle) return;
    g_nextTitle = g_time + 0.2;
    char buf[160];
    int n = snprintf(buf, sizeof buf, "<b>Enum</b>");
    if (g_home == 0 && g_cat >= 0) n += snprintf(buf + n, sizeof buf - n, " <size=14><color=grey>%s</color></size>", kCatName[g_cat]);
    if (f_fps) n += snprintf(buf + n, sizeof buf - n, "  <size=14>%d fps</size>", (int)(g_fps + 0.5f));
    SetText(g_title, buf);
}

static void ShowMenu(bool show) {
    if (show == g_menuShown) return;
    g_menuShown = show;
    if (show) { g_openTime = g_time; g_dirty = true; g_hover = -2; SetText(g_tipText, ""); }
    Show(g_menu, show);
    Show(g_pointer, show);
    g_touching = -2;
}

// The menu follows your hand, tipped 45 degrees, so you read it by turning your palm to your face.
static void PlaceMenu() {
    Il2CppObject *menu = g_menu.get(), *board = g_board.get(), *head = g_head.get();
    Il2CppObject *hand = f_rightHand ? g_rightHand.get() : g_leftHand.get();
    if (!Alive(menu) || !Alive(board) || !hand || !head) return;
    Il2CppObject *t = Tr(menu), *bt = Tr(board);
    Vector3 handPos = G.trGetPosition[hand]();
    if (!G.trGetRotation.IsValid()) {                  // fallback: float above the hand, facing you
        Vector3 center = handPos + Vector3(0, 0.25f, 0), away = center - G.trGetPosition[head]();
        if (Dot(away, away) < 1e-6f) return;
        G.trSetPosition[t](handPos);
        G.trSetPosition[bt](center);
        G.trSetRotation[bt](Quaternion::LookRotation(away));
        return;
    }
    const float s = 0.38268343f, c = 0.92387953f;                  // sin/cos of 22.5 degrees
    Quaternion rot = G.trGetRotation[hand]() * Quaternion(-s, 0, 0, c);   // tip -45 degrees on x
    if (f_rightHand) rot = rot * Quaternion(0, 0, 1, 0);           // mirrored: 180 degrees on z
    G.trSetPosition[t](handPos);
    G.trSetRotation[t](rot);
    G.trSetLocalPosition[bt](Vector3(0.1f, 0, 0));
    G.trSetLocalRotation[bt](Quaternion::LookRotation(Vector3(-1, 0, 0), Vector3(0, 0, 1)));
}

static void PressEntry(Entry *e) {
    if (e->kind == TOGGLE) {
        *e->flag = !*e->flag;
        if (e->fn) e->fn();
        Notify("[%s] %s", *e->flag ? "ON" : "OFF", e->label);
    } else if (e->fn) {
        e->fn();
    }
}
static void PressRow(Row &w) {
    if (w.e) PressEntry(w.e);
    else if (w.cat >= 0) { g_cat = w.cat; g_page = 0; }
    else { g_cat = -1; g_page = 0; }
}
static void ToggleFavorite(Entry *e) {
    bool &f = g_fav[EntryIndex(e)];
    f = !f;
    Notify("%s %s", f ? "Starred" : "Unstarred", e->label);
}

// Which part of the menu the pointer touches (see Hit), or H_NONE.
static int Touching() {
    Il2CppObject *board = g_board.get(), *pointer = g_pointer.get();
    if (!Alive(board) || !Alive(pointer)) return H_NONE;
    Vector3 p = G.trInverseTransformPoint[Tr(board)](G.trGetPosition[Tr(pointer)]());
    if (p.z < -0.035f || p.z > 0.015f) return H_NONE;
    if (PageCount() > 1 && fabsf(p.y - kFootY) < kNavH / 2) {
        if (fabsf(p.x + kNavX) < kNavW / 2) return H_PREV;
        if (fabsf(p.x - kNavX) < kNavW / 2) return H_NEXT;
    }
    if (fabsf(p.x - kResetX) < kResetW / 2 && fabsf(p.y - kHeaderY) < kResetH / 2) return H_RESET;
    if (fabsf(p.y - kTabY) < kTabH / 2)
        for (int t = 0; t < 2; t++) if (fabsf(p.x - (t ? kTabX : -kTabX)) < kTabW / 2) return 100 + t;
    if (fabsf(p.x) < kRowW / 2)
        for (int r = 0; r < kRows; r++)
            if (fabsf(p.y - RowY(r)) < kRowH / 2) return p.x > kStarX - kStarW / 2 ? 10 + r : r;
    return H_NONE;
}

static void Hover(int hit) {                              // the tip of the button under your finger
    const char *tip = "";
    if (hit == H_RESET) tip = "Switches every mod off.";
    else if (hit == 100) tip = "Your categories.";
    else if (hit == 101) tip = "Everything you starred, in one list.";
    else if (hit >= 0 && hit < 100) {
        int i = g_page * kRows + hit % 10;
        if (i < (int)g_view.size()) {
            Row &w = g_view[i];
            if (w.e) tip = hit >= 10 ? "Press the star to add it to Favorites." : w.e->tip;
            else if (w.cat >= 0) tip = kCatTip[w.cat];
            else tip = "Back to all categories.";
        }
    }
    SetText(g_tipText, tip);
}

static void CheckPress() {
    int hit = Touching();
    if (hit != g_hover) { g_hover = hit; Hover(hit); }
    if (hit != H_NONE && hit != g_touching && g_time >= g_nextPress) {   // one press per touch
        g_nextPress = g_time + 0.25;
        Buzz(f_rightHand ? 4 : 5);
        if (hit == H_PREV || hit == H_NEXT) {
            int n = PageCount();
            g_page = (g_page + (hit == H_NEXT ? 1 : n - 1)) % n;
        } else if (hit == H_RESET) {
            AllOff();
        } else if (hit >= 100) {
            g_home = hit - 100;
            g_page = 0;
        } else {
            int r = hit % 10, i = g_page * kRows + r;
            if (i < (int)g_view.size()) {
                Row &w = g_view[i];
                g_flashRow = r;
                g_flashUntil = g_time + 0.12;
                if (hit >= 10 && w.e) ToggleFavorite(w.e);      // the star on the right
                else PressRow(w);
            }
        }
        g_dirty = true;
    }
    g_touching = hit;
}

static void AnimateMenu() {
    float open = (float)(g_time - g_openTime) / 0.15f;     // pop open
    if (open <= 1.05f && Alive(g_menu.get())) {
        float k = (open >= 1 ? 1 : 1 - (1 - open) * (1 - open)) * kMenuScale;
        G.trSetLocalScale[Tr(g_menu.get())](Vector3(k, k, k));
    }
    if (g_flashRow != -2 && g_time >= g_flashUntil) { g_flashRow = -2; g_dirty = true; }
    float t = 0.5f + 0.5f * sinf((float)g_time * 2.0f);
    Color bg = f_rainbow ? Lerp(T().bgA, Rainbow((float)g_time * 0.15f), 0.35f) : Lerp(T().bgA, T().bgB, t);
    SetColor(g_bg.get(), bg);
    SetColor(g_outline.get(), f_rainbow ? Rainbow((float)g_time * 0.15f) : Lerp(T().buttonOn, T().title, t));   // a soft glow
}

// fps / clock line at the top of your view (twice a second, only while one of them is on)
static void HudTick() {
    bool want = f_fps || f_clock;
    if (!want) {
        if (g_nextHud != 0) { Show(g_hudGo, false); g_nextHud = 0; }
        return;
    }
    if (g_time < g_nextHud) return;
    g_nextHud = g_time + 0.5;
    char buf[64];
    int n = 0;
    if (f_fps) n += snprintf(buf + n, sizeof buf - n, "%d fps", (int)(g_fps + 0.5f));
    if (f_clock) {
        time_t now = time(nullptr);
        struct tm lt;
        localtime_r(&now, &lt);
        n += snprintf(buf + n, sizeof buf - n, "%s%d:%02d", n ? "   " : "", lt.tm_hour % 12 ? lt.tm_hour % 12 : 12, lt.tm_min);
    }
    SetText(g_hudText, buf);
    Show(g_hudGo, true);
}

// ---- movement (local only) ------------------------------------------------------------

static Il2CppObject *PlayerTransform() {
    Il2CppObject *player = g_player.get();
    return player ? G.compTransform[player]() : nullptr;
}
// Your body's velocity, through the game's own player functions.
static bool GetVel(Vector3 &v) {
    Il2CppObject *player = g_player.get();
    if (player && G.plGetVelocity.IsValid()) { v = G.plGetVelocity[player](); return true; }
    return false;
}
static void SetVel(Vector3 v) {
    Il2CppObject *player = g_player.get();
    if (player && G.plSetVelocity.IsValid()) G.plSetVelocity[player](v);
}
static void StopMoving() { SetVel(Vector3(0, 0, 0)); }
// Where your body is (the body collider), which is what teleports aim for.
static bool BodyPos(Vector3 &out) {
    Il2CppObject *player = g_player.get(), *t = PlayerTransform();
    if (!player || !t) return false;
    Il2CppObject *body = G.bodyCollider.IsValid() ? G.bodyCollider[player]() : nullptr;
    out = body ? G.trGetPosition[G.compTransform[body]()]() : G.trGetPosition[t]();
    return true;
}
static void TeleportPlayer(Vector3 pos) {
    Il2CppObject *player = g_player.get(), *t = PlayerTransform();
    Vector3 body;
    if (!player || !t || !BodyPos(body)) return;
    Vector3 target = pos - body + G.trGetPosition[t]();
    if (G.teleportTo.IsValid()) G.teleportTo[player](target);
    else G.trSetPosition[t](target);
    StopMoving();
}
static void SaveSpot() {
    if (BodyPos(g_spot)) { g_haveSpot = true; Notify("Spot saved"); }
}
static void GoToSpot() {
    if (!g_haveSpot) { Notify("Save a spot first"); return; }
    TeleportPlayer(g_spot);
    Notify("Teleported to your spot");
}

static void FlyTick(const Buttons &in, float dt) {
    Il2CppObject *head = g_head.get(), *t = PlayerTransform();
    if (!f_fly || !in.a || !head || !t) return;
    Vector3 pos = G.trGetPosition[t]() + G.trGetForward[head]() * ((f_fastFly ? 20.0f : 8.0f) * dt);
    G.trSetPosition[t](pos);
    StopMoving();
}
static void LowGravityTick(const Buttons &in, float dt) {
    Vector3 v;
    if (!f_lowGravity || f_noGravity || (f_fly && in.a) || !GetVel(v)) return;
    SetVel(v + Vector3(0, 9.81f * 0.7f * dt, 0));       // cancels 70% of gravity
}
static void PlatformFor(bool held, Ref &platform, Ref &hand) {
    if (!held || !f_platforms) {
        Kill(platform);
        return;
    }
    if (platform.get() || !hand.get()) return;               // already standing on one
    Il2CppObject *p = Cube(T().button, true);                // keeps its collider: you stand on it
    if (!p) return;
    // Hands only collide with the layers in the player's locomotionEnabledLayers, so the
    // platform goes on the first of those layers.
    static int layer = -2;
    if (layer == -2) {
        layer = -1;
        int mask = G.locomotionLayers.IsValid() && g_player.get() ? G.locomotionLayers[g_player.get()]() : 0;
        for (int i = 0; i < 32 && layer < 0; i++) if (mask & (1 << i)) layer = i;
    }
    if (layer >= 0 && G.goSetLayer.IsValid()) G.goSetLayer[p](layer);
    Il2CppObject *t = Tr(p);
    G.trSetPosition[t](G.trGetPosition[hand.get()]() - Vector3(0, 0.08f, 0));
    G.trSetLocalScale[t](Vector3(0.28f, 0.02f, 0.28f));
    platform.set(p);
}

// Speed Boost and No Gravity change your own player; turning them off puts the old
// values back. Checked every frame so they also come back after a room change.
static float g_oldJumpSpeed = 0, g_oldJumpMultiplier = 0;
static Il2CppObject *g_boosted = nullptr;      // which player has the boost (compared only)
static void ApplySpeedBoost() {
    Il2CppObject *player = g_player.get();
    bool ok = G.maxJumpSpeed.IsValid() && G.jumpMultiplier.IsValid();
    if (f_speedBoost && ok && player && g_boosted != player) {
        g_oldJumpSpeed = G.maxJumpSpeed[player]();
        g_oldJumpMultiplier = G.jumpMultiplier[player]();
        G.maxJumpSpeed[player] = 9.0f;
        G.jumpMultiplier[player] = 1.5f;
        g_boosted = player;
    } else if (!f_speedBoost && g_boosted) {
        if (ok && player == g_boosted) {
            G.maxJumpSpeed[player] = g_oldJumpSpeed;
            G.jumpMultiplier[player] = g_oldJumpMultiplier;
        }
        g_boosted = nullptr;
    }
}
static void ApplyGravity() {
    // The game's own "no gravity for a while", renewed every frame while it is on;
    // it wears off by itself once you turn it off.
    Il2CppObject *player = g_player.get();
    if (f_noGravity && player && G.plNoGravityFor.IsValid()) G.plNoGravityFor[player](0.25f);
}

// ---- camera view ----------------------------------------------------------------------
// A second camera draws into a small texture shown on a screen floating in front of you.
// It only exists while the Camera switch is on (it costs a little speed, like any camera).
// The screen is on layer 31 and the camera skips that layer, so it can't see itself.

static Ref g_camGo, g_cam, g_screen, g_camTex;
static bool g_camBuilt = false;
static int g_camMode = 0, g_camDist = 1, g_camZoom = 1, g_camSide = 2, g_camSize = 1;
static const char *kCamModes[] = {"Selfie", "Behind"};
static const float kCamDists[] = {1.5f, 2.5f, 4.0f};
static const char *kCamDistNames[] = {"Near", "Medium", "Far"};
static const float kCamFovs[] = {90.0f, 60.0f, 35.0f};
static const char *kCamZoomNames[] = {"Wide", "Normal", "Zoom"};
static const char *kCamSides[] = {"Left", "Center", "Right"};
static const float kCamSideX[] = {-0.32f, 0.0f, 0.32f};
static const float kCamSizes[] = {0.22f, 0.35f, 0.5f};
static const char *kCamSizeNames[] = {"Small", "Medium", "Large"};
static const char *CamViewName() { return kCamModes[g_camMode]; }
static const char *CamDistName() { return kCamDistNames[g_camDist]; }
static const char *CamZoomName() { return kCamZoomNames[g_camZoom]; }
static const char *CamSideName() { return kCamSides[g_camSide]; }
static const char *CamSizeName() { return kCamSizeNames[g_camSize]; }

static void DestroyCam() {
    Kill(g_screen);
    g_cam.clear();
    Kill(g_camGo);
    if (Alive(g_camTex.get()) && G.rtRelease.IsValid()) G.rtRelease(g_camTex.get());
    g_camTex.clear();
    g_camBuilt = false;
}
static bool BuildCam() {
    if (!G.camOk) { Notify("Camera view isn't available in this game build"); return false; }
    Il2CppObject *tex = G.rtGet(256, 256, 16);
    Il2CppObject *go = tex ? NewObject("MyModCamera") : nullptr;
    Il2CppObject *cam = go ? G.goAddComponent[go](G.tCamera) : nullptr;
    if (cam) G.camSetTarget[cam](tex);                   // right away, before it can draw to the screen
    Il2CppObject *quad = cam ? Prim(5, Color(1, 1, 1, 1), false) : nullptr;
    if (!tex || !go || !cam || !quad) {
        if (cam) G.behaviourSetEnabled[cam](false);
        if (go) G.destroy(go);
        if (quad) G.destroy(quad);
        if (tex) G.rtRelease(tex);
        ym::log("my-mod: camera view could not be built (tex %d go %d cam %d quad %d)", tex != nullptr, go != nullptr, cam != nullptr, quad != nullptr);
        Notify("Camera view failed on this build");
        return false;
    }
    G.camSetCull[cam](0x7FFFFFFF);                       // everything except layer 31
    G.camSetFov[cam](kCamFovs[g_camZoom]);
    if (auto *r = G.goGetComponent[quad](G.tRenderer)) if (Il2CppObject *m = G.rendererGetMaterial[r]()) G.matSetTexture[m](tex);
    if (G.goSetLayer.IsValid()) G.goSetLayer[quad](31);
    g_camTex.set(tex);
    g_camGo.set(go);
    g_cam.set(cam);
    g_screen.set(quad);
    g_camBuilt = true;
    return true;
}
static void ApplyCam() {
    if (f_cam) {
        if (!g_camBuilt && !BuildCam()) { f_cam = false; return; }
        if (Alive(g_cam.get())) G.behaviourSetEnabled[g_cam.get()](true);
        Show(g_screen, true);
    } else if (g_camBuilt) {
        if (Alive(g_cam.get())) G.behaviourSetEnabled[g_cam.get()](false);
        Show(g_screen, false);
    }
}
static void NextCamView() { g_camMode = (g_camMode + 1) % 2; }
static void NextCamDist() { g_camDist = (g_camDist + 1) % 3; }
static void NextCamZoom() {
    g_camZoom = (g_camZoom + 1) % 3;
    if (Alive(g_cam.get())) G.camSetFov[g_cam.get()](kCamFovs[g_camZoom]);
}
static void NextCamSide() { g_camSide = (g_camSide + 1) % 3; }
static void NextCamSize() { g_camSize = (g_camSize + 1) % 3; }

static void CamTick() {
    Il2CppObject *head = g_head.get(), *cam = g_cam.get(), *screen = g_screen.get();
    if (!f_cam || !g_camBuilt || !head || !Alive(cam) || !Alive(screen)) return;
    Vector3 hp = G.trGetPosition[head](), f = Flat(G.trGetForward[head]()), up(0, 1, 0), r = Cross(up, f);
    Vector3 cp;
    Quaternion cr;
    if (g_camMode == 0) {                                // in front of you, looking at you
        cp = hp + f * kCamDists[g_camDist] + Vector3(0, 0.1f, 0);
        cr = Quaternion::LookRotation(Norm(hp - cp));
    } else {                                             // behind you, over your shoulder, looking where you look
        cp = hp - f * kCamDists[g_camDist] + up * 0.5f + r * 0.25f;
        cr = Quaternion::LookRotation(Norm(f + Vector3(0, -0.15f, 0)));
    }
    Il2CppObject *ct = G.compTransform[cam]();
    G.trSetPosition[ct](cp);
    G.trSetRotation[ct](cr);
    // The screen floats ahead of your head, a little low, turned toward you.
    Vector3 sp = hp + f * 0.9f + r * kCamSideX[g_camSide] + Vector3(0, -0.2f, 0);
    Il2CppObject *st = Tr(screen);
    G.trSetPosition[st](sp);
    G.trSetRotation[st](Quaternion::LookRotation(Norm(sp - hp)));
    float s = kCamSizes[g_camSize];
    G.trSetLocalScale[st](Vector3(s, s, 1));
}

// ---- target practice (local only) ---------------------------------------------------------
// Three balls float around you. Touch one with your right hand to score and it jumps
// somewhere else. Nothing leaves your game.

static constexpr int kTargets = 3;
static Ref g_targets[kTargets];
static Vector3 g_tpos[kTargets];
static int g_score = 0, g_best = 0, g_diff = 1;
static double g_gameEnd = 0, g_nextGameText = 0;
static const float kRadius[] = {0.12f, 0.08f, 0.05f};
static const float kRoundTime[] = {45.0f, 30.0f, 20.0f};
static const char *kDiffNames[] = {"Easy", "Normal", "Hard"};
static const char *DifficultyName() { return kDiffNames[g_diff]; }
static const char *BestName() { static char buf[16]; snprintf(buf, sizeof buf, "%d", g_best); return buf; }
static void ResetBest() { g_best = 0; Notify("Best score reset"); }

static void PlaceTarget(int i) {
    Il2CppObject *head = g_head.get(), *o = g_targets[i].get();
    if (!head || !Alive(o)) return;
    Vector3 hp = G.trGetPosition[head](), f = Flat(G.trGetForward[head]()), r = Cross(Vector3(0, 1, 0), f);
    Vector3 p = hp + f * (0.45f + Rand01() * 0.25f) + r * ((Rand01() - 0.5f) * 0.9f) + Vector3(0, (Rand01() - 0.65f) * 0.6f, 0);
    g_tpos[i] = p;
    Il2CppObject *t = Tr(o);
    G.trSetPosition[t](p);
    float d = kRadius[g_diff] * 2;
    G.trSetLocalScale[t](Vector3(d, d, d));
    SetColor(o, i % 2 ? Color(1.0f, 0.82f, 0.25f, 1) : T().buttonOn);
    G.goSetActive[o](true);
}
static void HideTargets() { for (int i = 0; i < kTargets; i++) Show(g_targets[i], false); }
static void NextDifficulty() {
    g_diff = (g_diff + 1) % 3;
    if (f_game) { f_game = false; HideTargets(); Notify("Game stopped: difficulty changed"); }
}
static void ApplyGame() {
    if (!f_game) { HideTargets(); return; }
    if (!Alive(g_targets[0].get())) {                    // built once, then only moved and shown
        for (int i = 0; i < kTargets; i++) {
            Il2CppObject *o = Prim(0, T().bgB, false);
            if (o) g_targets[i].set(o);
        }
    }
    if (!Alive(g_targets[0].get())) { f_game = false; Notify("Game could not start"); return; }
    g_score = 0;
    g_gameEnd = g_time + kRoundTime[g_diff];
    g_nextGameText = 0;
    for (int i = 0; i < kTargets; i++) PlaceTarget(i);
    Notify("Touch the balls with your right hand");
}
static void EndGame() {
    f_game = false;
    HideTargets();
    bool record = g_score > g_best;
    if (record) g_best = g_score;
    char buf[64];
    snprintf(buf, sizeof buf, "Time! %d points%s", g_score, record ? " - new best!" : "");
    GameText(buf);
    g_notifyUntil = g_time + 3.0;
    g_dirty = true;
}
static void GameTick() {
    if (!f_game) return;
    if (g_time >= g_gameEnd) { EndGame(); return; }
    Il2CppObject *hand = g_rightHand.get();
    if (!hand) return;
    if (!g_menuShown) {                                  // while the menu is open the finger ball is in play
        Vector3 hp = G.trGetPosition[hand]();
        float reach = kRadius[g_diff] + 0.05f;
        for (int i = 0; i < kTargets; i++) {
            if (!Alive(g_targets[i].get())) continue;
            Vector3 d = hp - g_tpos[i];
            if (Dot(d, d) < reach * reach) {
                g_score++;
                Buzz(5);
                PlaceTarget(i);
            }
        }
    }
    if (g_time >= g_nextGameText) {                      // twice a second
        g_nextGameText = g_time + 0.5;
        char buf[48];
        snprintf(buf, sizeof buf, "Score %d   Time %d", g_score, (int)(g_gameEnd - g_time + 0.99));
        GameText(buf);
    }
}

// ---- fog (local only) ------------------------------------------------------------------------

static bool g_fogSaved = false, g_fogPending = false;
static bool g_oldFog = false;
static Color g_oldFogColor;
static float g_oldFogDensity = 0;
static int g_fogColor = 0, g_fogThick = 1;
static const Color kFogColors[] = {Color(0.75f, 0.80f, 0.90f, 1), Color(1.0f, 0.35f, 0.15f, 1), Color(0.20f, 0.55f, 1.0f, 1),
                                   Color(0.30f, 0.90f, 0.40f, 1), Color(0.70f, 0.30f, 1.0f, 1), Color(0.05f, 0.05f, 0.08f, 1)};
static const char *kFogColorNames[] = {"Mist", "Fire", "Sky", "Lime", "Violet", "Night"};
static const float kFogDensity[] = {0.03f, 0.08f, 0.2f};
static const char *kFogThickNames[] = {"Light", "Medium", "Heavy"};
static const char *FogColorName() { return kFogColorNames[g_fogColor]; }
static const char *FogThicknessName() { return kFogThickNames[g_fogThick]; }

// Turning fog on remembers what the scene had, and turning it off puts that back.
static void ApplyFog() {
    if (!G.fogOk) { if (f_fog) { f_fog = false; Notify("Fog isn't available in this game build"); } return; }
    if (f_fog) {
        if (!g_fogSaved) {
            g_oldFog = G.fogGet();
            g_oldFogColor = G.fogColorGet();
            g_oldFogDensity = G.fogDensityGet();
            g_fogSaved = true;
        }
        G.fogSet(true);
        G.fogColorSet(kFogColors[g_fogColor]);
        G.fogDensitySet(kFogDensity[g_fogThick]);
    } else if (g_fogSaved) {
        G.fogSet(g_oldFog);
        G.fogColorSet(g_oldFogColor);
        G.fogDensitySet(g_oldFogDensity);
        g_fogSaved = false;
    }
}
static void NextFogColor() { g_fogColor = (g_fogColor + 1) % 6; if (f_fog) ApplyFog(); }
static void NextFogThickness() { g_fogThick = (g_fogThick + 1) % 3; if (f_fog) ApplyFog(); }

// ---- everything off ---------------------------------------------------------------------------

static void AllOff() {
    for (auto &e : g_entries) if (e.kind == TOGGLE && e.flag != &f_notify && e.flag != &f_rightHand && e.flag != &f_rainbow) *e.flag = false;
    ApplySpeedBoost();
    ApplyCam();
    ApplyFog();
    HideTargets();
    Notify("Everything off");
}

// ---- callbacks ----------------------------------------------------------------------------------

// Forgets every menu object (the room that owned them is gone). Destroying the menu root
// also destroys everything parented under it; the rest are just released.
static void ForgetMenu() {
    for (int r = 0; r < kRows; r++) {
        g_rowCube[r].clear();
        for (int k = 0; k < 3; k++) { g_rowGo[r][k].clear(); g_rowTx[r][k].clear(); }
    }
    for (int t = 0; t < 2; t++) g_tabCube[t].clear();
    for (Ref *r : {&g_board, &g_bg, &g_outline, &g_title, &g_titleBar, &g_tipText, &g_countText, &g_navText,
                   &g_resetCube, &g_infoBar, &g_prev, &g_next, &g_notifyText, &g_hudText}) r->clear();
    for (Ref *r : {&g_menu, &g_pointer, &g_notifyGo, &g_hudGo, &g_leftPlatform, &g_rightPlatform}) Kill(*r);
}

static void OnRoomChange() {
    // The old room's objects are gone (or about to be): forget them, build again.
    ForgetMenu();
    for (int i = 0; i < kTargets; i++) Kill(g_targets[i]);
    DestroyCam();
    f_game = false;
    g_boosted = nullptr;
    g_fogSaved = false;                           // the new scene has its own fog settings
    g_fogPending = true;
    g_nextHud = 0;
    for (Ref *r : {&g_player, &g_leftHand, &g_rightHand, &g_head}) r->clear();
    g_built = false; g_menuShown = false; g_touching = -2; g_haveSpot = false;
}

static void OnUpdate(float dt) {
    g_time += dt;
    g_frames++;
    static double fpsStart = 0;
    if (g_time - fpsStart >= 1.0) { g_fps = (float)(g_frames / (g_time - fpsStart)); g_frames = 0; fpsStart = g_time; }
    if (!Lookups()) return;
    auto *player = (Il2CppObject *)ym::api()->localPlayer();
    if (!player) return;
    if (player != g_player.get()) {                 // first frame in this room
        OnRoomChange();
        g_player.set(player);
        g_leftHand.set(G.leftHand[player]());
        g_rightHand.set(G.rightHand[player]());
        g_head.set(G.headTransform[player]());
    }
    if (!g_leftHand.get() || !g_rightHand.get() || !g_head.get()) return;
    if (!g_built) BuildMenu();
    if (g_fogPending) { g_fogPending = false; ApplyFog(); }
    if (f_cam && !g_camBuilt) ApplyCam();          // camera is rebuilt after a room change

    Buttons in = ReadButtons();
    ShowMenu(f_rightHand ? in.b : in.y);
    if (g_menuShown) {
        PlaceMenu();
        CheckPress();
        if (g_dirty) RenderPage();
        AnimateMenu();
    }
    if (g_notifyUntil > 0 && g_time >= g_notifyUntil) { Show(g_notifyGo, false); g_notifyUntil = 0; }
    HudTick();
    FlyTick(in, dt);
    LowGravityTick(in, dt);
    PlatformFor(in.lgrip, g_leftPlatform, g_leftHand);
    PlatformFor(in.rgrip, g_rightPlatform, g_rightHand);
    ApplySpeedBoost();
    ApplyGravity();
    CamTick();
    GameTick();
}

YM_EXPORT int YeepsMod_Init(const YeepsModApi *api) {
    if (!ym::init(api)) return -1;
    ym::onUpdate(OnUpdate);
    ym::log("Enum loaded on %s (%s)", api->gameVersion, api->buildId);
    return 0;
}
