# Android UVC Field Monitor
## Capture Calibration Integrated Architecture & Design Specification

> Merged specification based on the original **Capture Calibration Design Specification** and **Capture Calibration Architecture Specification v1.1**.
>
> Where the two documents conflict, the later Architecture Specification v1.1 decision is adopted. Detailed measurement, patch, search, and validation content from the original Design Specification is retained unless explicitly superseded.

---

# 1. 目的

Android UVC Field Monitorにおいて、HDMI → UVCキャプチャ経路で発生する輝度・コントラスト・色差・色相・RGB成分間の誤差を実測し、再現性のある補正を適用するCapture Calibration機構を定義する。

本機能は表示デバイスそのものを校正するDisplay Calibrationではない。主として以下の経路を対象とする。

```text
HDMI Source
    ↓
HDMI Output
    ↓
Capture IC
    ↓
UVC YUV
    ↓
Android UVC Field Monitor
```

名称は **Capture Calibration** とする。

本システムは、未知の入力信号を完全自動解析することを目的としない。既知またはオペレータが選択した入力条件に対し、以下を明確に分離して扱う。

- Capture Device PUによる一次補正
- Software Calibrationによる残差補正
- HDR / SDR Signal Processing
- Optional User 3D LUT
- Preview / Scope表示

目標は以下。

- 24パッチテストパターンによる一括測定
- UVC Processing Unit（PU）を利用した一次補正
- PUで補正しきれない残差をソフトウェア補正
- HDR / SDR処理とは分離
- 自動キャリブレーションを約60秒以内で完了することを目標とする
- キャリブレーション完了後は設定を固定し、映像処理中の負荷と状態変化を最小化する

基本原則は以下とする。

> Measure first.  
> Correct only measured error.  
> Do not infer missing behavior.  
> Keep calibration deterministic.

---

# 2. 基本思想

補正は二段階で行う。

```text
Stage 1
UVC PUによるハードウェア側調整

brightness
contrast
saturation
hue

        ↓

Stage 2
Capture Calibrationによる残差補正

offset
3×3 RGB matrix
optional 1D correction
```

PUで補正可能な大きな誤差は可能な限りキャプチャデバイス側で吸収し、その後に残る個体差やRGBクロストーク、非線形誤差をソフトウェア側で補正する。

いきなり3D LUTへ全補正を押し込まず、補正要因を明確に分離する。

Capture CalibrationとUser 3D LUTも別機能として扱う。

```text
Capture Calibration
= Capture Device Correction

User 3D LUT
= Color Transform / Creative Transform
```

---

# 3. システム全体構成

Capture Calibration機能はField Monitor本体から完全に分離した別Applicationとして実装する。

ただし、UVC Capture、libusb、Descriptor Parser、PU Control等の低レイヤ処理は共通ライブラリとして共有する。

```text
Android Project
│
├─ app-monitor
│    └─ Android UVC Field Monitor
│
├─ app-calibration
│    └─ Capture Calibration
│
└─ native-uvc
     └─ Shared Native UVC Library
```

端末上では、

```text
Field Monitor
Capture Calibration
```

の2つの独立したAPKとして動作する。

## 3.1 Application Runtimeの分離

Field MonitorはCalibration実行状態を持たない。

Monitor Runtimeには以下を持ち込まない。

- Calibration Patch State
- PU Search State
- Measurement State
- Solver State
- Validation State
- Calibration UI
- Calibration用タイマー
- Calibration用USB State Machine

Monitorが扱うのは完成済みCalibration Profileのみとする。

```text
Calibration App
        │
        │ Calibration Profile
        ▼
Field Monitor
```

## 3.2 Source Codeの共有

Applicationは分離するが、共通機能をコピーして別実装にはしない。

特に以下は共通ライブラリ化する。

```text
USB Device Access
UVC Descriptor Parsing
VideoControl Interface Discovery
Processing Unit Discovery
PU Control Transfer
UVC Streaming Backend
YUYV handling
Common Device Information
```

これにより、Calibration Appで確認したUVC挙動とMonitor本体のUVC挙動を一致させる。

---

# 4. Android Project構成

初期段階では過剰なModule分割を行わない。

```text
Android-UVC/
│
├─ app-monitor/
├─ app-calibration/
└─ native-uvc/
```

将来的に必要になった場合のみ、

```text
native-color/
calibration-core/
profile-core/
```

等への分離を検討する。

これらは現時点では論理境界として扱い、必要になる前にModule化しない。

---

# 5. Signal Pipeline / キャリブレーション処理位置

Capture CalibrationはHDR / SDRの意味解釈より前に配置する。

```text
Known / Operator-selected Input Signal
                │
                ▼
              HDMI
                │
                ▼
        Capture Device
        ├─ Brightness
        ├─ Contrast
        ├─ Saturation
        └─ Hue
                │
                ▼
             UVC YUV
                │
                ▼
       Transport Decode
        ├─ YUV unpack
        ├─ Chroma reconstruction
        ├─ Range normalization
        └─ YCbCr → R'G'B'
                │
                ▼
       Capture Calibration
        ├─ Code-domain Offset
        ├─ Code-domain 3×3 Matrix
        └─ Optional 1D correction
                │
                ├──────────────→ Measurement / Scope
                │
                ▼
       Signal Interpretation
        ├─ SDR inverse transfer
        └─ HDR PQ EOTF
                │
                ▼
      Linear-light Processing
        ├─ Tone Mapping
        └─ Gamut Conversion
                │
                ▼
        Output Transfer Encode
                │
                ▼
       Optional User 3D LUT
                │
                ▼
             Preview
```

Capture Calibrationは、

> 入力されたコード値がキャプチャ経路でどの程度変形したか

のみを扱う。

PQ / Gammaなどの伝達特性そのものをCapture Calibration内部で解釈しない。

---

# 6. Capture Calibration Domain

Capture Calibrationが扱うRGBはLinear RGBではない。

```text
Encoded R'G'B'
Code-domain RGB
```

とする。

したがってCapture Calibration内部の3×3 Matrixは、

```text
Color Space Conversion Matrix
```

ではなく、

```text
Capture Path Error Correction Matrix
```

として扱う。

Capture Calibration自身は以下を解釈しない。

- PQ
- Gamma
- SDR / HDR
- Tone Mapping
- Creative Look

---

# 7. Signal Domain Contract

各処理段の意味を固定する。

## Stage 0

```text
UVC YUYV
Integer code values
```

## Stage 1

```text
Normalized Y'CbCr
Range interpreted
```

## Stage 2

```text
Encoded R'G'B'
Capture Calibration input
```

## Stage 3

```text
Calibrated Encoded R'G'B'
Measurement / Scope source
```

## Stage 4

```text
Linear RGB
SDR inverse transfer / PQ EOTF output
```

## Stage 5

```text
Linear Display RGB
Tone Mapping / Gamut Conversion output
```

## Stage 6

```text
Encoded Display RGB
Output Transfer applied
```

## Stage 7

```text
Display-referred RGB
Optional User 3D LUT input
```

## Stage 8

```text
Framebuffer / Android Display
```

各Shader、CPU処理、LUT処理は、自身がどのStageの値を入力・出力するか明示する。

---

# 8. Scope Pipeline

Waveform、RGB Parade等の測定系はPreview Pipelineとは分離する。

HDR PQ入力の場合、

```text
PQ Input
   │
   ├─ Capture Calibration
   │       │
   │       └─ Scope
   │           PQ code-domain measurement
   │
   └─ PQ EOTF
           ↓
       Tone Mapping
           ↓
        Preview
```

とする。

Preview用Tone Mapping後の値をSource Scopeへ使用しない。

Scopeは原則としてCapture Calibration後の入力信号値を観測する。

---

# 9. HDR / SDRと入力条件

Capture Calibration自体はHDR専用・SDR専用ではない。

ただし、以下の条件が変化するとキャプチャIC内部処理が変わる可能性がある。

```text
RGB Full
RGB Limited
YCbCr 4:4:4
YCbCr 4:2:2

BT.601
BT.709
BT.2020

SDR
HDR PQ
```

したがってHDR / SDRという分類だけではなく、

```text
Input Pixel Format
Range
Color Matrix / Colorimetry
Transfer Characteristics
Capture Device
```

をCalibration条件として扱う。

実機検証の結果、条件間で補正値に有意差がなければ共通Profile化する。

差が確認された場合は入力条件別にProfileを保持する。

---

# 10. 24パッチテストパターン

## 5.1 解像度

送信側テストパターンは以下を基準とする。

```text
1280 × 720
60p または 30p
```

24パッチを6列×4行で配置する。

```text
┌────┬────┬────┬────┬────┬────┐
│  1 │  2 │  3 │  4 │  5 │  6 │
├────┼────┼────┼────┼────┼────┤
│  7 │  8 │  9 │ 10 │ 11 │ 12 │
├────┼────┼────┼────┼────┼────┤
│ 13 │ 14 │ 15 │ 16 │ 17 │ 18 │
├────┼────┼────┼────┼────┼────┤
│ 19 │ 20 │ 21 │ 22 │ 23 │ 24 │
└────┴────┴────┴────┴────┴────┘
```

1280×720では理論上、

```text
1 Patch ≈ 213 × 180 pixel
```

UVC側が720×480になった場合でも、

```text
1 Patch ≈ 120 × 120 pixel
```

程度を確保できるため、ROI平均には十分なサイズとなる。

---

---

# 11. パッチ構成

初期案として以下の24色を使用する。

### Grayscale

```text
1   Black       0,   0,   0
2   Gray 10%   26,  26,  26
3   Gray 25%   64,  64,  64
4   Gray 50%  128, 128, 128
5   Gray 75%  192, 192, 192
6   Gray 90%  230, 230, 230
7   White      255, 255, 255
```

### Primary / Secondary

```text
8   Red       255,   0,   0
9   Green       0, 255,   0
10  Blue        0,   0, 255

11  Cyan        0, 255, 255
12  Magenta   255,   0, 255
13  Yellow    255, 255,   0
```

### Mid-level Primary / Secondary

```text
14  Mid Red      128,   0,   0
15  Mid Green      0, 128,   0
16  Mid Blue       0,   0, 128

17  Mid Cyan       0, 128, 128
18  Mid Magenta  128,   0, 128
19  Mid Yellow   128, 128,   0
```

### Low-level Primary / Validation

```text
20  Low Red       64,   0,   0
21  Low Green      0,  64,   0
22  Low Blue       0,   0,  64

23  Gray 40%     102, 102, 102
24  Gray 60%     153, 153, 153
```

最終的な24色構成については実測結果を見ながら変更可能とする。

---

---

# 12. Patch Role

24 Patchは初期Calibration Patternとして使用可能とする。

役割は概念上、

```text
Diagnostic patches
Calibration / Solve patches
Validation patches
```

に分離して考える。

Full-scale patchは、

- clipping
- range mismatch
- channel ceiling
- gross gain error

の検出に有効である。

ただしCapture Device内部ですでにclippingしている場合、Matrix Solveへの寄与は低下する可能性がある。

Full-scale patchをSolveへ含めるかどうかは実測後に判断する。

---

# 13. ROI測定

パッチ全体を平均せず、各パッチ中央部のみを測定する。

理由：

- YUV 4:2:2クロマ補間
- キャプチャIC内部スケーリング
- シャープネス
- フィルタリング
- パッチ境界のクロストーク

の影響を避けるため。

例：

```text
Captured Patch
120 × 120 pixel

        ↓

Measurement ROI
60 × 60 pixel
```

すなわち中央約50%を測定対象とする。

1 ROIあたり、

```text
60 × 60 = 3,600 pixels
```

24パッチでは、

```text
86,400 pixels
```

となり、CPU / GPUどちらで処理しても十分軽量である。

---

---

# 14. フレーム平均

単一フレームのみで補正値を決定しない。

推奨：

```text
PU変更
    ↓
3～5 frames discard
    ↓
8～16 frames measurement
    ↓
ROI average
```

USB転送、HDMIキャプチャ内部処理、ノイズなどによる瞬間的な揺れを平均化する。

30fpsの場合でも測定時間は十分短い。

---

---

# 15. UVC PU Calibration基準

対象PUは初期段階では以下の4項目とする。

```text
Brightness
Contrast
Saturation
Hue
```

旧仕様で使用していた、

```text
brightness = 0
contrast   = 128
saturation = 128
hue        = 0
```

は、特定デバイスでの初期確認値としては利用可能だが、全UVC Device共通の絶対的neutral値とは仮定しない。

可能な限りDeviceから以下を取得する。

```text
GET_INFO
GET_LEN
GET_MIN
GET_MAX
GET_RES
GET_DEF
GET_CUR
```

Calibration開始時の基準はDevice Defaultを優先し、Deviceが報告するcapabilityと実測結果から扱いを決定する。

過去の実機調整値を正解として無条件に引き継がず、測定から最適値を求める。

PUについては以下を区別する。

```text
Unsupported

Supported and effective

Supported but image does not change

Request failed

Value accepted but readback differs
```

---

# 16. PU Discovery / PU Raw Inspector

VID/PIDによるProcessing Unit IDの固定決め打ちは行わない。

VideoControl Interface内のClass-specific Descriptorを解析し、

```text
VC_PROCESSING_UNIT
```

を探索する。

最低限、

```text
bUnitID
bmControls
```

を取得する。

Calibration App最初の実機機能はPU Raw Inspectorとする。

```text
USB Device Open
        ↓
VideoControl Interface Discovery
        ↓
Processing Unit Discovery
        ↓
PU Unit ID
        ↓
bmControls
        ↓
GET_* Request
        ↓
Raw Value Display
```

初期段階では値を正規化しない。

例：

```text
Brightness

Supported : YES
GET_LEN   : 2
MIN       : -128
MAX       : 127
RES       : 1
DEF       : 0
CUR       : 0

RAW MIN   : 80 FF
RAW MAX   : 7F 00
```

数値解釈とRaw Byte列を両方保存・表示する。

---

# 17. PU自動調整順序

基本順序：

```text
Brightness
    ↓
Contrast
    ↓
Saturation
    ↓
Hue
    ↓
Brightness / Contrast 再確認
    ↓
Saturation / Hue 再確認
```

一度だけではなく、2パス方式を基本とする。

理由：

UVC Processing Unit内部で各パラメータが完全に独立している保証がないため。

これは実機検証項目とする。

---

---

# 18. 各PUの評価指標

## 11.1 Brightness

主に、

```text
Black
Gray 10%
Gray 25%
```

を評価する。

目的：

- Black offset
- Low level luminance error

の最小化。

---

## 11.2 Contrast

主にGray ramp全体を使用。

```text
Black
10%
25%
40%
50%
60%
75%
90%
White
```

期待値との差をRMSE等で評価する。

---

## 11.3 Saturation

RGB / CMYパッチを使用し、色差成分の大きさを評価する。

概念：

```text
Expected chroma magnitude
vs
Measured chroma magnitude
```

---

## 11.4 Hue

RGB / CMYパッチを利用し、色差ベクトル方向を評価する。

概念：

```text
Expected chroma angle
vs
Measured chroma angle
```

---

---

# 19. PU探索方式

全レンジ総当たりは行わない。

段階探索を使用する。

例：

```text
Coarse
±16

    ↓

Medium
±4

    ↓

Fine
±1
```

各PUにつき約5～8評価程度を目標とする。

---

---

# 20. 自動キャリブレーション処理

想定フロー：

```text
AUTO CAL START
       ↓
24 Patch Detection
       ↓
ROI Setup
       ↓
Reset PU to Device-derived baseline
       ↓
Brightness Search
       ↓
Contrast Search
       ↓
Saturation Search
       ↓
Hue Search
       ↓
Second Pass
       ↓
Final PU Lock
       ↓
24 Patch Measurement
       ↓
Capture Calibration Solve
       ↓
Validation
       ↓
Profile Commit
       ↓
AUTO CAL DONE
```

---

---

# 21. 目標処理時間

目標：

```text
Auto Calibration
< 60 seconds
```

推定では、

```text
PU optimization:
20～40 seconds

Capture Calibration solve:
<< 1 second
```

程度を想定する。

この時間は現段階では推定値であり、実際のUVC PU反映速度、フレーム安定時間、USB転送条件によって変化するため、実機測定で確定する。

---

---

# 22. Capture Calibration補正モデル

この補正モデルが扱うRGBは **Encoded R'G'B' code-domain values** とする。

PU調整後に残る誤差をソフトウェアで補正する。

初期実装では以下を想定。

```text
Input RGB
   ↓
Offset
   ↓
3×3 Matrix
   ↓
Optional 1D Tone Correction
   ↓
Corrected RGB
```

基本モデル：

```text
[R']   [m00 m01 m02] [R]   [Or]
[G'] = [m10 m11 m12] [G] + [Og]
[B']   [m20 m21 m22] [B]   [Ob]
```

未知数：

```text
3 × 3 Matrix = 9
Offset       = 3

Total        = 12 parameters
```

24パッチから取得できる観測値：

```text
24 × RGB
= 72 observations
```

最小二乗法で解く。

24点を使用するため、12未知数に対して十分な過剰決定系となる。

---

---

# 23. 1D Correction

Gray ramp測定で明確な非線形性が確認された場合のみ追加する。

```text
R 1D LUT
G 1D LUT
B 1D LUT
```

ただし初期実装では必須としない。

まず、

```text
PU
+
Offset
+
3×3 Matrix
```

でどこまで補正可能か確認する。

---

---

# 24. User 3D LUTとの関係

Capture Calibration用補正とUser 3D LUTは別機能として扱う。

```text
Capture Calibration
= Capture Device Correction

User 3D LUT
= Color Transform / Creative Transform
```

v1ではUser LUTのdomainを、

```text
Display-referred Encoded RGB
```

に限定する。

処理順は以下とする。

```text
UVC
↓
Capture Calibration
↓
HDR / SDR Signal Interpretation
↓
Linear-light Processing
↓
Output Transfer Encode
↓
User 3D LUT
↓
Display
```

Capture Calibration結果を`.cube`として保存することは必須としない。

内部補正は軽量なMatrix / 1D LUTを基本とする。

以下のLUT domain自動判別はv1対象外とする。

```text
Log → Rec.709
PQ → SDR
Scene Linear LUT
Automatic LUT Domain Detection
```

---

# 25. LUTローダー構想

将来的なUser 3D LUT対応では`.cube`形式を第一候補とする。

想定：

```text
.cube
↓
LUT Loader
↓
CPU parse
↓
Validation
↓
GL_TEXTURE_3D
↓
GPU sampling
```

33³ LUT：

```text
33 × 33 × 33
= 35,937 texels
```

Field Monitor用途では十分軽量と考えられる。

LUTは映像開始前にロードし、通常運用中は変更しない。

---

---

# 26. 起動時固定思想

映像処理開始後の動的な状態変更は極力避ける。

```text
Application Setup
    ↓
Input Mode
    ↓
Calibration Profile
    ↓
HDR / SDR Mode
    ↓
Optional User LUT
    ↓
USB Open
    ↓
EGL Init
    ↓
Shader Compile
    ↓
LUT Upload
    ↓
RUN
```

運用中は固定状態とする。

必要な設定変更は、

```text
STOP
↓
Configuration Change
↓
Renderer Reinitialize
↓
START
```

を基本とする。

目的：

- 状態遷移削減
- GL resource再生成削減
- 不具合箇所削減
- 再現性向上

性能向上そのものよりも、動作の決定性と保守性を優先する。

---

---

# 27. Calibration Profile

Calibration結果はProfileとして保存する。

最低限以下を保持する。

```text
Device
 ├─ VID
 ├─ PID
 └─ Serial / Unique ID if available

Capture Mode
 ├─ Pixel Format
 ├─ Width
 ├─ Height
 └─ Frame Rate

Input Contract
 ├─ Input Encoding
 ├─ Range
 ├─ Colorimetry
 └─ Transfer Characteristics

PU
 ├─ Brightness
 ├─ Contrast
 ├─ Saturation
 └─ Hue

Correction
 ├─ RGB Offset
 ├─ RGB 3×3 Matrix
 └─ Optional 1D LUT

Validation
 ├─ Error Before
 ├─ Error After PU
 └─ Error After Capture Calibration

Metadata
 ├─ Profile Format Version
 ├─ Calibration Model Version
 ├─ Pattern Version
 ├─ Solver Version
 ├─ Validation Result
 └─ Timestamp
```

Profileは単なる補正係数ではなく、その補正が成立するCapture Mode / Input Contractと組み合わせて扱う。

---

# 28. Profile Matching

同一VID/PIDだから同じProfileが使用可能とは仮定しない。

最低限、

```text
Pixel Format
Resolution
Input Encoding
Range
Colorimetry
Transfer Characteristics
```

をMatching条件として扱う。

Capture Modeが異なる場合、Capture IC内部処理経路が変わる可能性がある。

Profile mismatch時は補間・推測による適用を行わない。

```text
PROFILE_MODE_MISMATCH
Calibration disabled
```

実測により差が十分小さいことが確認された条件のみ、Profile統合を検討する。

---

# 29. Profile Commit / Application間受け渡し

Calibration作業中の値はWorking Profileとして扱う。

```text
Measurement
↓
PU Adjustment
↓
Matrix Solve
↓
Validation
↓
Profile Commit
```

Validation完了前に既存Profileを書き換えない。

保存は可能な限りAtomic Commitとし、Calibration失敗時は既存の正常Profileを保持する。

Calibration AppとMonitor Appは別APKとする。

初期実装では単純なProfile Export / Import方式を許容する。

```text
Calibration App
      ↓
Calibration Profile
      ↓
Export
      ↓
Monitor App
      ↓
Import
```

将来的に必要な場合のみContentProvider等による直接共有を検討する。

初期段階ではProfile内容を人間が確認可能な形式とすることを優先する。

---

# 30. キャリブレーション結果評価

補正前後の誤差を保存する。

例：

```text
Base PU
RMSE = 8.24

Adjusted PU
RMSE = 3.12

Adjusted PU
+ Capture Calibration
RMSE = 0.91
```

最低限、

```text
Before
After PU
After Capture Calibration
```

の3状態を比較可能とする。

---

---

# 31. Calibration App UI案

```text
┌──────────────────────────┐
│     Capture Calibration  │
│                          │
│ Pattern        DETECTED  │
│ Brightness     ✓         │
│ Contrast       ✓         │
│ Saturation     ✓         │
│ Hue            ✓         │
│ Capture Cal    ✓         │
│                          │
│ Error Before   8.24      │
│ Error After    0.91      │
│                          │
│ Completed      32.7 sec  │
│                          │
│       [ AUTO CAL ]       │
└──────────────────────────┘
```

このUIはField Monitor内の画面ではなく、独立したCapture Calibration App側に配置する。Field Monitor RuntimeへCalibration UIを持ち込まない。

---

---

# 32. テストパターン送信側

Windows側で1280×720テストパターンをフルスクリーン表示する。

重要事項：

Windows framebufferのRGB値とHDMI伝送上の値が必ず一致するとは限らない。

以下が影響する可能性がある。

```text
GPU Driver
RGB Full / Limited
YCbCr output
Windows HDR
ICC / Color Management
GPU scaling
HDMI output configuration
```

したがって本キャリブレーションで測定される対象は厳密には、

```text
Windows framebuffer
↓
GPU / OS output pipeline
↓
HDMI
↓
Capture Device
↓
UVC
```

という系全体となる。

特定のCapture IC単体特性のみを測定したい場合は、送信側信号条件を固定する必要がある。

---

---

# 33. Source変更時の扱い

理想的にはCapture Calibration ProfileはCapture Device単位で再利用する。

ただし、

```text
RGB Full → RGB Limited
RGB → YCbCr
BT.709 → BT.2020
SDR → HDR PQ
Pixel Format変更
Resolution変更
```

など入力条件またはCapture Modeが変わった場合、同じ補正係数が成立する保証はない。

そのためProfileは、

```text
Capture Device
+
Capture Mode
+
Input Encoding
+
Range
+
Colorimetry
+
Transfer Characteristics
```

を条件として扱う。

実測により差が十分小さいことが確認できた条件のみ統合する。

---

# 34. HDR Pipeline成立条件

HDR PQ処理を行う前に、Capture DeviceがPQ code valueを保持してUVCへ出力していることを確認する。

以下がCapture Device内部で実施されている場合、通常のPQ Pipelineは成立しない可能性がある。

```text
Internal Tone Mapping
Gamma Conversion
Gamut Conversion
Range Remapping
Unknown Nonlinear Processing
```

これはCapture Calibrationそのものではなく、HDR Pipeline成立条件の検証項目とする。

---

# 35. v1実装範囲と対象外

v1の最終的なCalibration機能として、以下を実装対象候補とする。

```text
24 Patch Display

ROI Measurement

UVC PU Capability Read

Brightness Auto Adjustment

Contrast Auto Adjustment

Saturation Auto Adjustment

Hue Auto Adjustment

Second Pass

Final 24 Patch Measurement

Offset + 3×3 Matrix Solve

Before / After Error Evaluation

Calibration Profile Save / Export
```

以下はv1必須要件としない。

```text
完全な入力信号自動認識

任意HDMI Source差の完全吸収

任意Resolutionへの完全自動追従

Automatic Geometry Detection

Capture Device内部処理の完全モデル化

3D LUT Capture Calibration

Complex / Nonlinear Gamut Correction

Live Calibration

Dynamic Calibration Switching

Dynamic LUT Switching

LUT Domain Auto Detection
```

ただし実装開始時点では上記v1全機能を一括実装しない。開発初期目標は後述のPhase 0 / Phase 1相当までに限定する。

---

# 36. 設計上の原則

本機能は以下を原則とする。

```text
Measure first.
Correct only measured error.
Do not infer missing behavior.
Keep calibration deterministic.
```

つまり、

- 実測されていない誤差を推測して補正しない
- Capture CalibrationとHDR処理を混同しない
- Capture Device補正とCreative LUTを混同しない
- 自動補正後はパラメータを固定する
- 補正前後を数値で検証可能にする

ことを重視する。

---

---

# 37. 全体構成

```text
                    ┌──────────────────────┐
                    │ Windows Pattern Gen. │
                    │ 1280×720 / 24 Patch │
                    └──────────┬───────────┘
                               │ HDMI
                               ▼
                    ┌──────────────────────┐
                    │    Capture Device    │
                    │ Brightness           │
                    │ Contrast             │
                    │ Saturation           │
                    │ Hue                  │
                    └──────────┬───────────┘
                               │ UVC
                               ▼

          ┌─────────────────────────────────────────┐
          │          Capture Calibration App        │
          │                                         │
          │ shared native-uvc                       │
          │   ├─ USB / UVC                          │
          │   ├─ Descriptor / PU Discovery          │
          │   └─ PU Control                         │
          │                                         │
          │ Measurement                             │
          │   ↓                                     │
          │ PU Optimization                         │
          │   ↓                                     │
          │ Offset + 3×3 Matrix Solve               │
          │   ↓                                     │
          │ Validation                              │
          │   ↓                                     │
          │ Calibration Profile Commit              │
          └───────────────────┬─────────────────────┘
                              │
                              │ Profile Export / Import
                              ▼
          ┌─────────────────────────────────────────┐
          │              Field Monitor App          │
          │                                         │
UVC ─────→│ shared native-uvc                       │
          │      ↓                                  │
          │ Transport Decode                        │
          │      ↓                                  │
          │ Capture Calibration Apply ─────→ Scope │
          │      ↓                                  │
          │ Signal Interpretation                   │
          │      ↓                                  │
          │ Linear-light Processing                 │
          │      ↓                                  │
          │ Output Transfer Encode                  │
          │      ↓                                  │
          │ Optional User 3D LUT                    │
          │      ↓                                  │
          │ Preview                                 │
          └─────────────────────────────────────────┘
```

Calibration実行系とMonitor Runtimeは分離するが、UVC low-level implementationは共有する。

---

# 38. 今後の実機検証項目

以下は現時点では確定事項ではなく、実測で確認する。

1. PU変更後に必要な安定待ちFrame数
2. Brightness / Contrast / Saturation / Hue間の相互干渉
3. RGB / YCbCr入力時の補正値差
4. BT.709 / BT.2020入力時の補正値差
5. SDR / PQでのCapture Calibration Profile共有可否
6. Offset + 3×3 Matrixのみで十分か
7. 1D LUT追加による改善量
8. 24 Patch構成の最適化
9. Full-scale patchをMatrix Solveへ含めるべきか
10. Solve / Validation patch分離の必要性
11. RGB-domain Calibration以外のモデル検討が必要か
12. Capture DeviceがHDR入力をinternally tone-mapしていないか
13. Pixel Format / Resolution変更によるCapture内部経路差
14. 自動探索の収束時間
15. 60秒以内のAuto Calibration達成可否

これらは推測で固定せず、実機測定結果をもとに仕様化する。

---

# 39. 開発初期目標

現在の最優先目標はAuto Calibration完成ではない。

以下を順番に確認する。

```text
1 Project
↓
2 Application Modules
↓
Shared native-uvc
↓
Both Apps Build / Link
↓
Calibration App Launch
↓
PU Discovery
↓
PU Raw Value Acquisition
```

最初の実機到達点は、

> Capture Calibration App単独で、UVC Processing UnitからPUの生値を取得できること

とする。

この時点ではまだ以下を実装しない。

- PU自動調整
- Patch解析
- Matrix Solve
- HDR処理
- MonitorへのCalibration適用

機能を一気に実装せず、各段階で単独確認してから次へ進む。

---

# 40. 最終運用目標

最終的な操作イメージは以下。

```text
HDMI接続
↓
24 Patch表示
↓
Capture Calibration App起動
↓
[AUTO CAL]
↓
PU調整
↓
Capture Calibration生成
↓
Validation PASS
↓
Profile Commit / Export
↓
Field Monitor AppへProfile適用
↓
映像監視開始
```

Auto Calibrationの処理時間は約60秒以内を目標とするが、現段階では保証値ではない。UVC PU反映速度、Frame安定時間、USB転送条件等を実機測定した上で確定する。

ユーザーが個別にBrightness / Contrast / Saturation / Hueを追い込む作業を原則不要とし、Capture Device交換時でも短時間で再現性の高い補正状態を構築できることを最終目標とする。
