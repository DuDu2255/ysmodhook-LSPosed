package de.robv.android.xposed;

/** 编译期桩：运行时由 LSPosed 提供真实实现（读模块自身的 SharedPreferences） */
public class XSharedPreferences {
    public XSharedPreferences(String packageName, String prefFileName) {}
    public XSharedPreferences(String packageName) {}
    public boolean getBoolean(String key, boolean defValue) { return defValue; }
    public int getInt(String key, int defValue) { return defValue; }
    public String getString(String key, String defValue) { return defValue; }
    public void reload() {}
}
