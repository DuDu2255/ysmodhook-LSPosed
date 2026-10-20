package com.ysmod.hook;

import android.util.Log;

import java.io.File;

import de.robv.android.xposed.IXposedHookLoadPackage;
import de.robv.android.xposed.callbacks.XC_LoadPackage.LoadPackageParam;

/**
 * LSPosed 入口：只在目标游戏进程里加载 native hook 库。
 * native 部分与 Zygisk 版完全一致（Vulkan draw call 拦截）。
 */
public class Entry implements IXposedHookLoadPackage {

    private static final String TAG = "ysmod";
    private static final String TARGET = "com.miHoYo.Yuanshen";

    @Override
    public void handleLoadPackage(LoadPackageParam lpparam) {
        if (lpparam == null || !TARGET.equals(lpparam.packageName)) return;

        try {
            String so = findLib();
            if (so == null) {
                Log.e(TAG, "libysmod.so not found in module apk");
                return;
            }
            System.load(so);
            Log.i(TAG, "loaded " + so);
        } catch (Throwable t) {
            Log.e(TAG, "load failed: " + t);
        }
    }

    /** 从模块 APK 的路径推出已解压的 native 库目录 */
    private String findLib() {
        String apk = null;
        try {
            java.security.CodeSource cs = Entry.class.getProtectionDomain().getCodeSource();
            if (cs != null && cs.getLocation() != null) apk = cs.getLocation().getPath();
        } catch (Throwable ignored) {}

        if (apk == null) return null;
        File apkFile = new File(apk);
        File base = apkFile.getParentFile();          // /data/app/~~xx/com.ysmod.hook-yy/
        if (base == null) return null;

        String[] abis = {"arm64", "arm64-v8a"};
        for (String abi : abis) {
            File f = new File(new File(base, "lib"), abi + "/libysmod.so");
            if (f.exists()) return f.getAbsolutePath();
        }
        // 兜底：APK 内直接解压（extractNativeLibs=false 的情况）
        File direct = new File(base, "lib/arm64/libysmod.so");
        return direct.exists() ? direct.getAbsolutePath() : null;
    }
}

