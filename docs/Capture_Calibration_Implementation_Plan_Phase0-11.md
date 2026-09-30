# Capture Calibration
## Implementation Plan Phase 0-11

## 1. 開発方針

Capture Calibrationは段階的に実装する。

各Phaseは、

```text
実装
↓
単体確認
↓
実機確認
↓
完了判定
↓
次Phase
```

の順序で進める。

複数Phaseを同時に実装しない。

特に、

```text
USB
PU
Measurement
Solver
HDR
Monitor Integration
```

を同時に変更しない。

不具合発生時に原因レイヤを特定できる状態を維持する。

---

# Phase 0
## Project Separation / Shared Library Link

### 目的

Calibration AppをField MonitorからApplicationとして完全分離する。

同時に、UVC Low-level処理を共有可能な構造を作る。

### 構成

```text
Android-UVC/
│
├─ app-monitor/
├─ app-calibration/
└─ native-uvc/
```

### 実装範囲

- `app-calibration` Application Module作成
- Monitorと異なる`applicationId`
- Calibration App単独起動
- `native-uvc` Library作成
- Monitor / Calibration双方から同一Libraryをlink
- 仮Test Symbolを両Appからcall

例：

```cpp
int uvc_library_test()
{
    return 123;
}
```

### このPhaseでは行わない

- libusb移植
- UVC streaming
- Descriptor解析
- PU取得
- Profile
- Calibration UI

### 完了条件

```text
app-monitor build OK

app-calibration build OK

両方が同じnative-uvcをlink

Calibration App単独起動
```

### Phase 0 Gate

既存MonitorがPhase 0以前と同じ状態で動作すること。

---

# Phase 1
## PU Raw Inspector

### 目的

Capture Calibration AppからUVC Processing Unitの生情報を取得する。

現在の最優先Phase。

### 実装順

```text
USB Device Open
↓
VideoControl Interface Discovery
↓
Processing Unit Descriptor Discovery
↓
Unit ID取得
↓
bmControls取得
↓
PU GET Request
↓
Raw Value表示
```

### 初期対象

```text
Brightness
Contrast
Saturation
Hue
```

### 取得値

```text
GET_INFO
GET_LEN
GET_MIN
GET_MAX
GET_RES
GET_DEF
GET_CUR
```

### 表示

数値解釈とRaw Bytesの両方を表示する。

例：

```text
Brightness

Supported : YES

LEN : 2

MIN : -128
MAX : 127
RES : 1
DEF : 0
CUR : 0

RAW MIN : 80 FF
RAW MAX : 7F 00
```

### このPhaseでは行わない

- SET_CUR
- PU自動探索
- 映像評価
- Calibration Pattern
- Matrix

### 完了条件

4 PUについて、

```text
Supported / Unsupported
MIN
MAX
RES
DEF
CUR
```

を実機から取得できること。

### Phase 1成果物

```text
PU Raw Inspector
```

---

# Phase 2
## PU Write / Readback Verification

### 目的

PU値を書き換え、Deviceが実際に値を受理しているか確認する。

### 処理

```text
GET_CUR
↓
SET_CUR
↓
GET_CUR
↓
Readback比較
```

### 確認内容

```text
Request Success

Requested Value

Readback Value

Value Quantization

GET_RES準拠

Out-of-range rejection
```

### さらに確認

映像に本当に変化があるかを確認する。

以下を区別する。

```text
Unsupported

SET_CUR rejected

SET_CUR accepted
but readback differs

Value changes
but image unchanged

Value changes
and image changes
```

### 完了条件

Brightness / Contrast / Saturation / Hueを個別に変更し、挙動を記録できる。

---

# Phase 3
## Measurement Engine

### 目的

Calibration Patternから安定した測定値を取得できるようにする。

### 実装

```text
Frame Acquisition
↓
Patch ROI
↓
Frame Accumulation
↓
RGB Mean / Statistics
```

### 初期機能

- 固定Geometry
- 固定ROI
- 8〜16 Frame平均
- Mean RGB
- Min / Max
- Standard Deviation

### このPhaseでは行わない

- PU自動調整
- Matrix Solve
- Automatic Geometry Detection

### 完了条件

同じPatternを連続入力したとき、十分再現性のある測定値を得られること。

### 評価対象

```text
Frame-to-frame noise
ROI contamination
Capture stability
```

---

# Phase 4
## PU Auto Calibration

### 目的

PUのみで補正可能な一次誤差を自動補正する。

### 基本順序

```text
Brightness
↓
Contrast
↓
Saturation
↓
Hue
↓
Brightness / Contrast再確認
↓
Saturation / Hue再確認
```

2-pass構成とする。

### 各Step

```text
PU SET
↓
Settling Frames discard
↓
Measurement Frames
↓
Error Evaluation
↓
Next PU Value
```

### 未確定項目

以下は実測で決定する。

```text
Settling Frame Count

Measurement Frame Count

Search Algorithm

PU interaction

Search convergence condition
```

### 完了条件

同じDevice /同じ入力条件で複数回実行し、ほぼ同じPU値へ収束すること。

---

# Phase 5
## RGB Offset + 3×3 Matrix Solver

### 目的

PUで補正できない残差をSoftware Calibrationで補正する。

### モデル

```text
[Rc]   [m00 m01 m02] [R]   [Or]
[Gc] = [m10 m11 m12] [G] + [Og]
[Bc]   [m20 m21 m22] [B]   [Ob]
```

### Solve

24 Patch等から、

```text
72 observations
12 parameters
```

の過剰決定系を解く。

### 必要機能

```text
Least Squares Solver

Residual Calculation

Condition Number

Singular / unstable detection
```

### 完了条件

Known Synthetic Dataで既知Matrixを復元できる。

次に実機データで補正前後のResidualが低下する。

---

# Phase 6
## Validation Engine

### 目的

Calibrationが「数値上解けた」だけでなく、実際に有効か判定する。

### 指標

最低限以下を記録する。

```text
Mean Absolute Error

Maximum Channel Error

Neutral Axis Error

Primary Error

Secondary Error

Black Offset

White Error

Clipping Detection

Matrix Condition Number
```

### 将来候補

```text
Solve Patches
Validation Patches
```

の分離。

### 状態例

```text
CALIBRATION_VALID

CALIBRATION_POOR_FIT

CALIBRATION_UNSTABLE

CALIBRATION_CLIPPED

CALIBRATION_FAILED
```

### 完了条件

補正値を保存してよいか、数値指標から判定できる。

---

# Phase 7
## Calibration Profile

### 目的

実測結果を再現可能なProfileとして保存する。

### 保存情報

```text
Device Identity

Capture Mode

Input Contract

PU Values

RGB Offset

RGB Matrix

Optional 1D LUT

Calibration Metadata

Validation Results
```

### 必須Matching条件

```text
VID / PID

Pixel Format

Width / Height

Input Encoding

Range

Colorimetry

Transfer Characteristics
```

Serialが取得可能な場合はDevice Identityに追加する。

### Commit

```text
Working Profile
↓
Validation
↓
Atomic Commit
```

失敗時に既存Profileを破壊しない。

### 完了条件

Calibration Appを終了・再起動してもProfileを再読込できる。

---

# Phase 8
## Field Monitor Profile Apply

### 目的

Calibration Appが生成したProfileをMonitor Runtimeで適用する。

### Monitor側処理

```text
Capture
↓
Profile Match
↓
PU Fixed Values
↓
YUV → R'G'B'
↓
Offset + 3×3 Matrix
↓
Scopes / Signal Processing
```

### Monitor側で禁止すること

```text
Calibration Search

Matrix Solve

Patch Recognition

Calibration State Machine
```

### Mismatch時

推測適用しない。

例：

```text
PROFILE_MODE_MISMATCH

Calibration disabled
```

### 完了条件

同一映像に対してCalibration AppとMonitorで同一補正結果になる。

---

# Phase 9
## SDR / HDR Integration

### 目的

Capture CalibrationとSignal Interpretationを統合する。

### SDR

```text
Calibrated R'G'B'
↓
Inverse Transfer
↓
Linear Processing
```

### HDR PQ

```text
Calibrated PQ R'G'B'
↓
PQ EOTF
↓
Linear-light
↓
Tone Mapping
↓
Gamut Transform
```

### Scope

```text
Calibrated Code-domain Signal
```

を測定する。

### 実機確認

Capture DeviceがHDR信号に対して、

```text
Internal Tone Mapping
Gamma Conversion
Unknown Gamut Conversion
```

を行っていないか確認する。

### 完了条件

PQ Patternのcode valueとScope値の関係が設計通り成立する。

---

# Phase 10
## Optional User 3D LUT

### 目的

Capture Calibrationとは独立したUser Color Transformを追加する。

### v1 Domain

```text
Display-referred
Encoded RGB
```

に限定する。

### Pipeline

```text
Linear Processing
↓
Output Transfer Encode
↓
User 3D LUT
↓
Preview
```

### v1対象外

```text
Automatic LUT Domain Detection

Log LUT Auto Detection

PQ LUT Auto Detection

Dynamic LUT Switching
```

### 完了条件

User LUT ON/OFFがCapture Calibration結果へ影響しない。

---

# Phase 11
## Performance / QA / Release Validation

### 目的

Calibration機能を実運用可能な状態へ確定する。

### Calibration Performance

評価する。

```text
PU Search Time

Measurement Time

Matrix Solve Time

Validation Time

Total Calibration Time
```

`< 60 seconds`は現時点では目標値であり保証値ではない。

実測後に仕様確定する。

### Monitor Performance

確認する。

```text
Calibration Matrix GPU Cost

Frame Time

Presentation Latency

Scope Performance

Profile Load Cost
```

### Regression

最低限、

```text
Calibration OFF

Calibration ON

HDR OFF

HDR ON

User LUT OFF

User LUT ON
```

の組み合わせを確認する。

### Device Matrix

少なくとも対象Deviceごとに確認する。

```text
MS2109
MS2109S
MS2130
```

各Deviceで同じPU仕様を仮定しない。

### 最終成果物

```text
Calibration Profile Format v1

PU Capability Report

Supported Capture Modes

Known Limitations

Calibration Timing Result

Regression Test Result
```

---

# 実装優先順位

現在は以下だけを対象とする。

```text
Phase 0
Project / Module separation
Shared native link
        ↓
Phase 1
PU Raw Inspector
```

Phase 2以降はPhase 1完了後まで着手しない。

直近ゴールは、

> Capture Calibration Appから、実機UVC DeviceのProcessing Unitを検出し、Brightness / Contrast / Saturation / Hueの生値を読めること。

ここまで到達した時点で、一度実測結果を確認してから次の設計判断を行う。
