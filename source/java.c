#include <falso_jni/FalsoJNI.h>
#include <falso_jni/FalsoJNI_Impl.h>
#include <falso_jni/FalsoJNI_Logger.h>

#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#define VITA_SCREEN_W 960
#define VITA_SCREEN_H 544
#define VITA_DPI 220.0f

static char fake_context;
static char fake_surface;
static char fake_file;
static char fake_asset_manager;
static char fake_display_metrics;
static char clipboard_text[1024];

static jobject new_string(const char *value) {
    return jni->NewStringUTF(&jni, value ? value : "");
}

static jobject new_int_array_4(jint a, jint b, jint c, jint d) {
    jint values[4] = { a, b, c, d };
    jintArray array = jni->NewIntArray(&jni, 4);
    if (array)
        jni->SetIntArrayRegion(&jni, array, 0, 4, values);
    return array;
}

static void consume_string_arg(va_list args) {
    (void)va_arg(args, jstring);
}

static jobject getNativeSurface(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return &fake_surface;
}

static jobject getContext(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return &fake_context;
}

static jobject getFileObject(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return &fake_file;
}

static jobject getExternalFilesDir(jmethodID id, va_list args) {
    (void)id;
    (void)va_arg(args, jobject);
    return &fake_file;
}

static jobject getAbsolutePath(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return new_string(DATA_PATH);
}

static jobject getAssets(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return &fake_asset_manager;
}

static jobject getDisplayDPI(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return &fake_display_metrics;
}

static jobject emptyIntArray(jmethodID id, va_list args) {
    (void)id;
    (void)va_arg(args, jint);
    return jni->NewIntArray(&jni, 0);
}

static jobject openAPKExpansionInputStream(jmethodID id, va_list args) {
    (void)id;
    consume_string_arg(args);
    return NULL;
}

static jobject clipboardGetText(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return new_string(clipboard_text);
}

static jobject audioOpen(jmethodID id, va_list args) {
    (void)id;
    jint sample_rate = va_arg(args, jint);
    jint format = va_arg(args, jint);
    jint channels = va_arg(args, jint);
    jint frames = va_arg(args, jint);
    return new_int_array_4(sample_rate, format, channels, frames);
}

static jobject captureOpen(jmethodID id, va_list args) {
    (void)id;
    (void)va_arg(args, jint);
    (void)va_arg(args, jint);
    (void)va_arg(args, jint);
    (void)va_arg(args, jint);
    return NULL;
}

static jboolean returnTrue(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return JNI_TRUE;
}

static jboolean returnFalse(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return JNI_FALSE;
}

static jboolean setActivityTitle(jmethodID id, va_list args) {
    (void)id;
    consume_string_arg(args);
    return JNI_TRUE;
}

static jboolean sendMessage(jmethodID id, va_list args) {
    (void)id;
    (void)va_arg(args, jint);
    (void)va_arg(args, jint);
    return JNI_TRUE;
}

static jboolean showTextInput(jmethodID id, va_list args) {
    (void)id;
    (void)va_arg(args, jint);
    (void)va_arg(args, jint);
    (void)va_arg(args, jint);
    (void)va_arg(args, jint);
    return JNI_FALSE;
}

static jboolean clipboardHasText(jmethodID id, va_list args) {
    (void)id;
    (void)args;
    return clipboard_text[0] != '\0';
}

static jboolean setCustomCursor(jmethodID id, va_list args) {
    (void)id;
    (void)va_arg(args, jint);
    return JNI_FALSE;
}

static jboolean setRelativeMouseEnabled(jmethodID id, va_list args) {
    (void)id;
    (void)va_arg(args, jint);
    return JNI_FALSE;
}

static jint createCustomCursor(jmethodID id, va_list args) {
    (void)id;
    (void)va_arg(args, jobject);
    (void)va_arg(args, jint);
    (void)va_arg(args, jint);
    (void)va_arg(args, jint);
    (void)va_arg(args, jint);
    return 0;
}

static jint captureRead(jmethodID id, va_list args) {
    (void)id;
    (void)va_arg(args, jobject);
    (void)va_arg(args, jint);
    return 0;
}

static void noopVoid(jmethodID id, va_list args) {
    (void)id;
    (void)args;
}

static void setWindowStyle(jmethodID id, va_list args) {
    (void)id;
    (void)va_arg(args, jint);
}

static void setOrientation(jmethodID id, va_list args) {
    (void)id;
    (void)va_arg(args, jint);
    (void)va_arg(args, jint);
    (void)va_arg(args, jint);
    consume_string_arg(args);
}

static void clipboardSetText(jmethodID id, va_list args) {
    (void)id;
    jstring text = va_arg(args, jstring);
    const char *chars = jni->GetStringUTFChars(&jni, text, NULL);
    if (!chars)
        chars = "";
    strncpy(clipboard_text, chars, sizeof(clipboard_text) - 1);
    clipboard_text[sizeof(clipboard_text) - 1] = '\0';
    jni->ReleaseStringUTFChars(&jni, text, (char *)chars);
}

static void hapticRun(jmethodID id, va_list args) {
    (void)id;
    (void)args;
}

enum {
    MID_GET_NATIVE_SURFACE = 100,
    MID_SET_ACTIVITY_TITLE,
    MID_SET_WINDOW_STYLE,
    MID_SET_ORIENTATION,
    MID_GET_CONTEXT,
    MID_RETURN_FALSE,
    MID_MANUAL_BACK_BUTTON,
    MID_INPUT_GET_INPUT_DEVICE_IDS,
    MID_SEND_MESSAGE,
    MID_SHOW_TEXT_INPUT,
    MID_CLIPBOARD_SET_TEXT,
    MID_CLIPBOARD_GET_TEXT,
    MID_CLIPBOARD_HAS_TEXT,
    MID_OPEN_APK_EXPANSION_INPUT_STREAM,
    MID_GET_MANIFEST_ENVIRONMENT_VARIABLES,
    MID_GET_DISPLAY_DPI,
    MID_CREATE_CUSTOM_CURSOR,
    MID_SET_CUSTOM_CURSOR,
    MID_SET_RELATIVE_MOUSE_ENABLED,
    MID_AUDIO_OPEN,
    MID_AUDIO_WRITE,
    MID_AUDIO_CLOSE,
    MID_CAPTURE_OPEN,
    MID_CAPTURE_READ,
    MID_CAPTURE_CLOSE,
    MID_POLL_INPUT_DEVICES,
    MID_HAPTIC_RUN,
    MID_HAPTIC_STOP,
    MID_GET_FILES_DIR,
    MID_GET_EXTERNAL_FILES_DIR,
    MID_GET_ABSOLUTE_PATH,
    MID_GET_ASSETS,
};

NameToMethodID nameToMethodId[] = {
    { MID_GET_NATIVE_SURFACE, "getNativeSurface", METHOD_TYPE_OBJECT },
    { MID_SET_ACTIVITY_TITLE, "setActivityTitle", METHOD_TYPE_BOOLEAN },
    { MID_SET_WINDOW_STYLE, "setWindowStyle", METHOD_TYPE_VOID },
    { MID_SET_ORIENTATION, "setOrientation", METHOD_TYPE_VOID },
    { MID_GET_CONTEXT, "getContext", METHOD_TYPE_OBJECT },
    { MID_RETURN_FALSE, "isTablet", METHOD_TYPE_BOOLEAN },
    { MID_RETURN_FALSE, "isAndroidTV", METHOD_TYPE_BOOLEAN },
    { MID_RETURN_FALSE, "isChromebook", METHOD_TYPE_BOOLEAN },
    { MID_RETURN_FALSE, "isDeXMode", METHOD_TYPE_BOOLEAN },
    { MID_RETURN_FALSE, "isScreenKeyboardShown", METHOD_TYPE_BOOLEAN },
    { MID_RETURN_FALSE, "supportsRelativeMouse", METHOD_TYPE_BOOLEAN },
    { MID_MANUAL_BACK_BUTTON, "manualBackButton", METHOD_TYPE_VOID },
    { MID_INPUT_GET_INPUT_DEVICE_IDS, "inputGetInputDeviceIds", METHOD_TYPE_OBJECT },
    { MID_SEND_MESSAGE, "sendMessage", METHOD_TYPE_BOOLEAN },
    { MID_SHOW_TEXT_INPUT, "showTextInput", METHOD_TYPE_BOOLEAN },
    { MID_CLIPBOARD_SET_TEXT, "clipboardSetText", METHOD_TYPE_VOID },
    { MID_CLIPBOARD_GET_TEXT, "clipboardGetText", METHOD_TYPE_OBJECT },
    { MID_CLIPBOARD_HAS_TEXT, "clipboardHasText", METHOD_TYPE_BOOLEAN },
    { MID_OPEN_APK_EXPANSION_INPUT_STREAM, "openAPKExpansionInputStream", METHOD_TYPE_OBJECT },
    { MID_GET_MANIFEST_ENVIRONMENT_VARIABLES, "getManifestEnvironmentVariables", METHOD_TYPE_BOOLEAN },
    { MID_GET_DISPLAY_DPI, "getDisplayDPI", METHOD_TYPE_OBJECT },
    { MID_CREATE_CUSTOM_CURSOR, "createCustomCursor", METHOD_TYPE_INT },
    { MID_SET_CUSTOM_CURSOR, "setCustomCursor", METHOD_TYPE_BOOLEAN },
    { MID_SET_CUSTOM_CURSOR, "setSystemCursor", METHOD_TYPE_BOOLEAN },
    { MID_SET_RELATIVE_MOUSE_ENABLED, "setRelativeMouseEnabled", METHOD_TYPE_BOOLEAN },
    { MID_AUDIO_OPEN, "audioOpen", METHOD_TYPE_OBJECT },
    { MID_AUDIO_WRITE, "audioWriteByteBuffer", METHOD_TYPE_VOID },
    { MID_AUDIO_WRITE, "audioWriteShortBuffer", METHOD_TYPE_VOID },
    { MID_AUDIO_WRITE, "audioWriteFloatBuffer", METHOD_TYPE_VOID },
    { MID_AUDIO_CLOSE, "audioClose", METHOD_TYPE_VOID },
    { MID_CAPTURE_OPEN, "captureOpen", METHOD_TYPE_OBJECT },
    { MID_CAPTURE_READ, "captureReadByteBuffer", METHOD_TYPE_INT },
    { MID_CAPTURE_READ, "captureReadShortBuffer", METHOD_TYPE_INT },
    { MID_CAPTURE_READ, "captureReadFloatBuffer", METHOD_TYPE_INT },
    { MID_CAPTURE_CLOSE, "captureClose", METHOD_TYPE_VOID },
    { MID_POLL_INPUT_DEVICES, "pollInputDevices", METHOD_TYPE_VOID },
    { MID_POLL_INPUT_DEVICES, "pollHapticDevices", METHOD_TYPE_VOID },
    { MID_HAPTIC_RUN, "hapticRun", METHOD_TYPE_VOID },
    { MID_HAPTIC_STOP, "hapticStop", METHOD_TYPE_VOID },
    { MID_GET_FILES_DIR, "getFilesDir", METHOD_TYPE_OBJECT },
    { MID_GET_EXTERNAL_FILES_DIR, "getExternalFilesDir", METHOD_TYPE_OBJECT },
    { MID_GET_ABSOLUTE_PATH, "getAbsolutePath", METHOD_TYPE_OBJECT },
    { MID_GET_ABSOLUTE_PATH, "getCanonicalPath", METHOD_TYPE_OBJECT },
    { MID_GET_ASSETS, "getAssets", METHOD_TYPE_OBJECT },
};

MethodsBoolean methodsBoolean[] = {
    { MID_SET_ACTIVITY_TITLE, setActivityTitle },
    { MID_RETURN_FALSE, returnFalse },
    { MID_SEND_MESSAGE, sendMessage },
    { MID_SHOW_TEXT_INPUT, showTextInput },
    { MID_CLIPBOARD_HAS_TEXT, clipboardHasText },
    { MID_GET_MANIFEST_ENVIRONMENT_VARIABLES, returnFalse },
    { MID_SET_CUSTOM_CURSOR, setCustomCursor },
    { MID_SET_RELATIVE_MOUSE_ENABLED, setRelativeMouseEnabled },
};
MethodsByte methodsByte[] = {};
MethodsChar methodsChar[] = {};
MethodsDouble methodsDouble[] = {};
MethodsFloat methodsFloat[] = {};
MethodsInt methodsInt[] = {
    { MID_CREATE_CUSTOM_CURSOR, createCustomCursor },
    { MID_CAPTURE_READ, captureRead },
};
MethodsLong methodsLong[] = {};
MethodsObject methodsObject[] = {
    { MID_GET_NATIVE_SURFACE, getNativeSurface },
    { MID_GET_CONTEXT, getContext },
    { MID_INPUT_GET_INPUT_DEVICE_IDS, emptyIntArray },
    { MID_CLIPBOARD_GET_TEXT, clipboardGetText },
    { MID_OPEN_APK_EXPANSION_INPUT_STREAM, openAPKExpansionInputStream },
    { MID_GET_DISPLAY_DPI, getDisplayDPI },
    { MID_AUDIO_OPEN, audioOpen },
    { MID_CAPTURE_OPEN, captureOpen },
    { MID_GET_FILES_DIR, getFileObject },
    { MID_GET_EXTERNAL_FILES_DIR, getExternalFilesDir },
    { MID_GET_ABSOLUTE_PATH, getAbsolutePath },
    { MID_GET_ASSETS, getAssets },
};
MethodsShort methodsShort[] = {};
MethodsVoid methodsVoid[] = {
    { MID_SET_WINDOW_STYLE, setWindowStyle },
    { MID_SET_ORIENTATION, setOrientation },
    { MID_MANUAL_BACK_BUTTON, noopVoid },
    { MID_CLIPBOARD_SET_TEXT, clipboardSetText },
    { MID_AUDIO_WRITE, noopVoid },
    { MID_AUDIO_CLOSE, noopVoid },
    { MID_CAPTURE_CLOSE, noopVoid },
    { MID_POLL_INPUT_DEVICES, noopVoid },
    { MID_HAPTIC_RUN, hapticRun },
    { MID_HAPTIC_STOP, noopVoid },
};

// System-wide constant that applications sometimes request.
char WINDOW_SERVICE[] = "window";

// Android 4.4 / KitKat keeps SDL on conservative code paths.
const int SDK_INT = 19;

NameToFieldID nameToFieldId[] = {
    { 0, "WINDOW_SERVICE", FIELD_TYPE_OBJECT },
    { 1, "SDK_INT", FIELD_TYPE_INT },
    { 2, "mSeparateMouseAndTouch", FIELD_TYPE_BOOLEAN },
    { 3, "density", FIELD_TYPE_FLOAT },
    { 4, "scaledDensity", FIELD_TYPE_FLOAT },
    { 5, "xdpi", FIELD_TYPE_FLOAT },
    { 6, "ydpi", FIELD_TYPE_FLOAT },
    { 7, "widthPixels", FIELD_TYPE_INT },
    { 8, "heightPixels", FIELD_TYPE_INT },
};

FieldsBoolean fieldsBoolean[] = {
    { 2, JNI_FALSE },
};
FieldsByte fieldsByte[] = {};
FieldsChar fieldsChar[] = {};
FieldsDouble fieldsDouble[] = {};
FieldsFloat fieldsFloat[] = {
    { 3, 1.0f },
    { 4, 1.0f },
    { 5, VITA_DPI },
    { 6, VITA_DPI },
};
FieldsInt fieldsInt[] = {
    { 1, SDK_INT },
    { 7, VITA_SCREEN_W },
    { 8, VITA_SCREEN_H },
};
FieldsObject fieldsObject[] = {
    { 0, WINDOW_SERVICE },
};
FieldsLong fieldsLong[] = {};
FieldsShort fieldsShort[] = {};

__FALSOJNI_IMPL_CONTAINER_SIZES
