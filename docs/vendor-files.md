# Vendor files

These Black Box files are in the repository root but **not in git** (`.gitignore`).
They are copyrighted. Keep them locally, or download them again from Black Box.

| File | Content | Used for |
|---|---|---|
| `BlackBox_Multi-Monitor_1.13_beta_July23-2021_27940.zip` | Windows multi-monitor driver 1.13 beta: NSIS installer (`mumoHID.sys`, `mumoMin.sys`, `mumomou.sys`, `GlideAndSwitch.exe`), release notes | Driver analysis (`windows-driver-analysis.md`) |
| `Glide and Switch Software 1.10_26153.zip` | Configuration application 1.10, help, `default.ffc`, firmware notes | Layout configuration |
| `Glide-And-Switch-Configuration.exe` | Configuration application (also in the 1.10 zip) | Layout configuration (Windows VM) |
| `GlideAndSwitch.chm` | Help file (also in the 1.10 zip) | `.ffc` and layout rules |
| `default.ffc` | Default layout: 4 computers, 1 screen each, 1920×1080 | `.ffc` format |
| `Firmware Revision Information.doc` | Firmware release notes | Version history |
| `KV0004A-R2_Firmware_2.10.8665_Oct5-2021_28196.zip` | Firmware 2.10.8665 (`KV0004AR2_V2.10.8665.bin`), adds copy and paste with a host driver | Installed version. Not analyzed. |
| `KV0004A-R2_Firmware_V2.05.8619_27781.zip` | Firmware 2.05.8619 | Previous version |
| `Setup_x64.exe`, `Setup_x86.exe` | Windows installers. Not identified (probably the copy-and-paste driver of firmware 2.10). | Not analyzed |

Safe handling: unpack each archive into its own new directory, and run analysis
scripts with `python3 -I` from a different directory.
