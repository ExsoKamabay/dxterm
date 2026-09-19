# JNI entry points are resolved by name; keep the native bridge class intact.
-keep class com.xdrac.NativeTerminal { *; }
# The VHDP bridge registers natives by name in JNI_OnLoad and constructs VhdpResult from C++.
-keep class com.xdrac.vhdp.VhdpNative { *; }
-keep class com.xdrac.vhdp.VhdpResult { *; }
-keepclasseswithmembernames class * { native <methods>; }
