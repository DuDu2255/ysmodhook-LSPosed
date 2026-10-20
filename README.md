# ysmodhook-LSPosed

原神（Android）Vulkan 绘制层 hook 模块，LSPosed 版本。
拦截 vkCmdDrawIndexed，采集索引缓冲 hash，为 3DMigoto / GIMI 风格的模型替换提供定位。

## 原理

    LSPosed 把本模块加载进游戏进程
      → Entry.handleLoadPackage() 判断包名
      → System.load(模块 APK 里的 libysmod.so)
      → native 侧 hook libvulkan 的 vkGetDeviceProcAddr / vkGetInstanceProcAddr
      → 拦截 vkCmdDrawIndexed / vkCmdBindIndexBuffer / vkBindBufferMemory
      → 对索引缓冲算 FNV-1a 32，按 hash 去重输出到 logcat

不修改游戏 APK，不写游戏文件，只做进程内拦截。

## 构建

推送到 main 分支后由 GitHub Actions 自动构建，产物在 Actions 的 Artifacts 里（ysmod-lsposed）。

本地构建（需要 Android SDK + NDK）：

    gradle assembleDebug

## 安装

1. 安装 APK
2. 在 LSPosed 里启用模块，作用域勾选 原神（com.miHoYo.Yuanshen）
3. 强制停止原神，重新启动

## 看日志

    adb logcat -s ysmod

或 root 下：

    su -c logcat -s ysmod

输出示例：

    ysmod : v0.7-lsp starting (pid=12345)
    ysmod : vkGetDeviceProcAddr: hooked 0x...
    ysmod : intercept(i) vkCmdDrawIndexed
    ysmod : [hash] 3ef08385 indexCount=16170 firstIndex=0 vtxOff=0

## 状态

- [x] 注入
- [x] Vulkan draw call 拦截
- [x] 索引缓冲 hash 采集
- [ ] hash 与 GIMI mod 对齐验证
- [ ] .buf 解析
- [ ] buffer 替换
- [ ] 描边 / 阴影 pass

## 说明

xposed-stub 模块只是编译期的 API 桩，不会打进 APK；运行时由 LSPosed 提供真实实现。

