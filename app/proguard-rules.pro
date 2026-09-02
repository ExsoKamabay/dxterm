# JNI entry points are resolved by name; keep the native bridge class intact.
-keep class com.xdrac.NativeTerminal { *; }
-keepclasseswithmembernames class * { native <methods>; }
