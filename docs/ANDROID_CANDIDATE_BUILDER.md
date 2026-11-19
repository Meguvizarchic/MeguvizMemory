# Android 工程候选构建器

`tools/build_android_candidate.py` 用固定的源码快照构建 Android 候选包。每种 ABI 各有一份 AAR 和一份原生 SDK，三组制品收在一个本地 ZIP 中，可用于应用集成或继续组装通用 AAR。项目采用 Apache-2.0 许可证，分发时一并保留 [LICENSE](../LICENSE) 和 [NOTICE](../NOTICE)。

## 输入与运行

先将目标提交完整导出到独立目录，例如 `build/frozen-source`；快照须包含本工具及 `tools/package_feature_preview.py`、`tools/source_lock.py`，其内容必须与执行时的这三份代码完全一致。生成快照时保留构建所需的 `CMakeLists.txt`、`src`、`include`、`cmake`、`adapters/android` 和 LLVM notice。不要在本仓库根目录直接用作源码快照：本工具要求工作目录不与源码目录重叠。

使用现有工具生成锁，锁文件不要放在快照中：

```text
python tools/source_lock.py --root build/frozen-source --output build/frozen-source-lock.json
```

然后提供本机已有 NDK、平台 `android.jar`、JDK、CMake、Ninja 和只存在于 `build/` 内的私有标识列表。以下尖括号表示由调用者提供的实际路径：

```text
python tools/build_android_candidate.py --source-root build/frozen-source --source-lock build/frozen-source-lock.json --ndk "<NDK目录>" --android-jar "<平台android.jar>" --jdk "<JDK目录>" --cmake "<cmake可执行文件>" --ninja "<ninja可执行文件>" --private-identifiers-file build/private-identifiers.txt --work-dir build/android-candidate-work --output dist/MeguvizMemory-Android-Candidate.zip
```

私有标识文件每行一个标识，不能为空；其值不写入输出。`--work-dir` 必须是 `build/` 下不存在的新目录；输出必须是 `dist/` 下不存在的新 ZIP。解析后的 `build/` 和 `dist/` 必须仍在工作区内。工具在开始前核验锁中的全部本项目文件及文件集合，复制到私有工作目录后再核验；core、JNI 和 Java 都从该副本构建，结束时再次核对快照、原始快照、锁、执行脚本和选定工具的哈希。源码快照和工具链在运行期间应保持静止。锁可含其他项目记录，但本工具只核验和编译 MeguvizMemory 文件，不宣称核验外部记录。

每个 ABI 使用 API 27、静态 C++ runtime、Release、共享 Memory core；不构建或安装诊断程序。`javac -h` 生成的 JNI 声明强制包含在 JNI 的实际编译单元中，以便编译器检查这些声明与原生定义的参数和返回类型。AAR 所含 class 来自同一次 Java 编译，SDK 与 AAR 的 core 和 JNI 字节逐项核对。

需要从 native 侧注册受控借用或压力控制器的 Android 消费者，可直接请求同一 SDK 中可选 JNI 目标：

```cmake
find_package(MeguvizMemory CONFIG REQUIRED COMPONENTS android_jni)
target_link_libraries(my_native_consumer PRIVATE MeguvizMemory::android_jni)
```

目标自动关联同一安装前缀的共享 `MeguvizMemory::mgmem`。它只在 Android 工具链、ABI 元数据和 core/JNI ELF 架构匹配时可用；已有或新导入的同名目标若来自其他前缀、包含额外链接接口或使用不安全的配置映射则拒绝，避免混用 Memory runtime。若导入后的目标不符合合同，包会报告 NOT FOUND；CMake 无法移除已创建目标，调用方必须停止使用该失败结果。未安装 JNI 的老 SDK 仍可无组件地使用 `find_package(MeguvizMemory CONFIG REQUIRED)`；显式要求缺失组件或错误 ABI 会失败。此目标不会在 Windows/Linux 宿主配置中暴露 Android `.so`。这些属性和当前配置映射只在包载入时检查；调用方随后修改 imported target、替换二进制或运行时 loader 搜索顺序不受静态检查保证。正常配对不表示支持动态卸载或证明设备运行，native 句柄与 Java 必须使用同一加载的 JNI 和核心 runtime。

core 的 `IMPORTED_LINK_INTERFACE_LIBRARIES` 通用及适用配置属性也必须为空：在现有 NDK/CMake 策略的生成链接命令中，这些旧式接口确实能传播额外库。相同对照中 `IMPORTED_LINK_DEPENDENT_LIBRARIES_RELEASE` 未改变链接命令，因此没有据此声称其他 CMake 策略或平台均无影响，也未将它作为本检查的拒绝条件。

## 安全处理和边界

- core/JNI 去掉调试和非加载 `.comment` 段前后，检查 ELF 目标 ABI、加载段字段及掩去 section-table 元数据后的加载字节、动态符号、依赖与重定位；每个加载段要求至少 16 KiB 对齐并满足其自身对齐值的地址与文件偏移同余。JNI 必需方法须为已定义、`GLOBAL`、`DEFAULT` 的可导出函数，且不允许遗留未解析的 pressure 符号。该检查是静态条件，不证明真实 16 KiB loader 运行。
- 静态 pressure 库去掉调试和 `.comment` 段前后，对比其已分配段、ARM attributes、非调试重定位及公开定义；除不占文件数据的 `SHT_NOBITS` 外，所选段须读到完整声明长度的数据。候选 SDK 中包含同一次构建的 pressure 库。AAR 的 JNI 只依赖该 ABI 的单一共享 Memory core 和允许的系统库。
- 校验 Java/JNI 名称、生成头、关键类、keep 规则、LLVM notice、AAR/SDK 成员与 core 身份。随后对 ZIP、嵌套 AAR/JAR、成员名、元数据和内容进行有界递归路径与私有标识扫描；它不能证明不存在所有秘密。
- 对每个解包后的脱敏 SDK，使用 Android NDK 的 `find_package(... COMPONENTS android_jni)` 编译并链接独立 native 测试组件，检查导入目标的文件路径和动态依赖。另检查 core-only SDK 的原有查找行为，以及缺 JNI、错误 ABI、错误 ELF 和重复引入不同前缀时的拒绝；这些只是编译和静态检查。
- 所有原始二进制、快照、标准输出、标准错误和逐步记录留在 `build/` 的工作目录；首步执行前独占创建 journal，后续检查其文件身份；步骤名必须唯一，日志文件以独占方式创建，发布前逐项核对日志哈希与记录。命令超时或失败时保留已有日志和失败状态。完成所有检查后以无替换原子移动写入 `dist/`，不存在与工作临时文件共享的硬链接。Windows 使用不覆盖现有文件的原子移动；Linux 需要支持 `renameat2(RENAME_NOREPLACE)` 且工作目录与 `dist/` 在同一文件系统，否则安全失败。不要分享整个工作目录。

候选中的 `SOURCE.json` 包含全部本项目源码记录及其可重算哈希；`MANIFEST.json` 记录工具哈希、逐步结果摘要、类、二进制和三个 ABI 的检查。哈希不是签名的完整构建来源证明；加载段与符号对照也不构成行为证明。尚未由本工具执行 R8 运行、Android ART、真实设备、MTE、能耗、性能或 BIL 验证。

无需 NDK 或设备即可运行打包工具的离线合同测试：

```text
python tools/test_build_android_candidate.py
```
