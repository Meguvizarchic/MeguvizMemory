# Android Maven 发布

`mgmem-android` 的 Gradle 发布链：把已验证的候选 ZIP（`dist/MeguvizMemory-Android-Candidate-*.zip`）
经 `tools/merge_android_candidate_aar.py` 合为三 ABI 通用 AAR，再由 AGP 打包成
`io.github.jichuo1.meguviz:mgmem-android:1.2.1` 发布到本地暂存仓库。

## 步骤

```powershell
cd bindings/android
# 1. 生成 wrapper 锁状态（首次）
.\gradlew.bat :mgmem-android:tasks --offline --write-locks

# 2. 组装（会先跑 mergeUniversalAar + extractJni）
.\gradlew.bat :mgmem-android:assembleRelease `
  "-Pmgmem.candidateZip=D:/DevArea/MeguvizMemory/dist/MeguvizMemory-Android-Candidate-mgmembb.zip" `
  "-Pmgmem.candidateSha256=d05b16eec51fd5c8568223832459196581340b05ac0b986df00680cec4d922a2" `
  --offline

# 3. 发布到本地暂存仓库（build/central-staging，不进公开仓库）
.\gradlew.bat :mgmem-android:publishCentralPublicationToCentralStagingRepository `
  "-Pmgmem.candidateZip=D:/DevArea/MeguvizMemory/dist/MeguvizMemory-Android-Candidate-mgmembb.zip" `
  "-Pmgmem.candidateSha256=d05b16eec51fd5c8568223832459196581340b05ac0b986df00680cec4d922a2" `
  --offline
```

暂存产物结构（`../../build/central-staging/io/github/jichuo1/meguviz/mgmem-android/1.2.1/`）：
`.aar` + `.pom` + `-sources.jar` + `-javadoc.jar` + `.module` 及各自校验和。
POM 含 Apache-2.0 许可、developer、SCM，无本地路径。

## 签名与上传

- 签名：`-Pmeguviz.sign=true`（本机 gpg-agent 交互式，密钥绝不进仓库）；不加时为未签名暂存包。
- 上传：暂存仓库签好后，按 Central Portal 要求上传 ZIP；验收用 `sha1` 逐文件对照。
- 版本位：`gradle.properties` 的 `meguviz.version=1.2.1` 为唯一版本源，
  与各 CMake 桥版本一致；改版只改这一处即可。

## 边界

- `local.properties`（SDK 路径）不入库，各本机自备。
- AGP 只打包不重编 native：`.so` 字节来自 merge 产物，与候选 ZIP 一致。
