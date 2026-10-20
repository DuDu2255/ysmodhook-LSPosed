package de.robv.android.xposed.callbacks;

/** 编译期桩：运行时由 LSPosed 提供真实实现 */
public class XC_LoadPackage {
    public static class LoadPackageParam {
        public String packageName;
        public ClassLoader classLoader;
        public Object appInfo;
        public boolean isFirstApplication;
        public String processName;
    }
}

