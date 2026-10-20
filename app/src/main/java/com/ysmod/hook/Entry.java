package com.ysmod.hook;

import android.util.Log;

import java.io.File;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.callbacks.XC_LoadPackage.LoadPackageParam;

/**
 * LSPosed 入口：只在目标游戏进程里加载并启动 native hook 库。
 * native 部分与 Zygisk 版完全一致（Vulkan draw call 拦截）。
 */
public class Entry implements IXposedHookLoadPackage {

    private static final String TAG = "ysmod";
    private static final String TARGET = "com.miHoYo.Yuanshen";
    private static final String SELF = "com.ysmod.hook";

    private static native void start();

    @Override
    public void handleLoadPackage(LoadPackageParam lpparam) {
        if (lpparam == null || !TARGET.equals(lpparam.packageName)) return;

        String apk = findApkPath();
        Log.i(TAG, "module apk = " + apk);
        if (apk == null) { Log.e(TAG, "cannot locate module apk"); return; }

        File base = new File(apk).getParentFile();
        if (base == null) { Log.e(TAG, "bad apk path"); return; }

        String[] abis = {"arm64", "arm64-v8a"};
        for (String abi : abis) {
            File f = new File(new File(base, "lib"), abi + "/libysmod.so");
            Log.i(TAG, "try " + f.getAbsolutePath() + " exists=" + f.exists());
            if (f.exists()) {
                try {
                    System.load(f.getAbsolutePath());
                    Log.i(TAG, "loaded ok, calling start()");
                    start();
                    return;
                } catch (Throwable t) {
                    Log.e(TAG, "load/start failed: " + t);
                }
            }
        }
        Log.e(TAG, "libysmod.so not found");
    }

    /** 三级兜底：CodeSource -> ClassLoader 字符串 -> PackageManager */
    private String findApkPath() {
        try {
            java.security.CodeSource cs = Entry.class.getProtectionDomain().getCodeSource();
            if (cs != null && cs.getLocation() != null) {
                String p = cs.getLocation().getPath();
                if (p != null && p.endsWith(".apk")) return p;
            }
        } catch (Throwable ignored) {}

        try {
            String s = Entry.class.getClassLoader().toString();
            Matcher m = Pattern.compile("(/data/app/[^\\[,\\s]+\\.apk)").matcher(s);
            if (m.find()) return m.group(1);
            Log.i(TAG, "classloader: " + s);
        } catch (Throwable ignored) {}

        try {
            Class<?> helper = Class.forName("android.app.AndroidAppHelper");
            android.app.Application app =
                    (android.app.Application) helper.getMethod("currentApplication").invoke(null);
            if (app != null) {
                android.content.pm.ApplicationInfo ai =
                        app.getPackageManager().getApplicationInfo(SELF, 0);
                if (ai != null && ai.sourceDir != null) return ai.sourceDir;
            }
        } catch (Throwable t) {
            Log.i(TAG, "pm fallback failed: " + t);
        }
        return null;
    }
}

