# JNI entry points are resolved by name; keep the native bridge class intact.
-keep class com.dracxterm.NativeTerminal { *; }
# The VHDP bridge registers natives by name in JNI_OnLoad and constructs VhdpResult from C++.
-keep class com.dracxterm.vhdp.VhdpNative { *; }
-keep class com.dracxterm.vhdp.VhdpResult { *; }
-keepclasseswithmembernames class * { native <methods>; }
