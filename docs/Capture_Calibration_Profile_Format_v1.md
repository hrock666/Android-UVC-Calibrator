# Capture Calibration Profile Format v1

## 目的

Capture Calibration Appで確定したPU値、RGBオフセット、3×3行列、検証結果を、Field Monitorへ受け渡せるJSONとして保存する。

## Commit条件

- Phase 5の行列Solveが完了していること
- Phase 6の最新検証結果が`CALIBRATION_VALID`または`CALIBRATION_POOR_FIT`であること
- USBセッションが接続中で、Brightness / Contrast / Saturation / Hueの現在値を再取得できること
- Input Encoding、Range、Colorimetry、Transfer Characteristicsがすべてユーザーにより明示的に選択されていること

Input Contractにデフォルト値は設定しない。未選択項目がある場合はCommitおよびExportを開始しない。

`CALIBRATION_POOR_FIT`は測定値の微調整や実機比較に使用できるよう、警告を表示したうえで保存を許可する。品質状態は`validation.result`にそのまま記録する。`CALIBRATION_UNSTABLE`、`CALIBRATION_CLIPPED`および検証未実施の場合は新しいProfileを保存せず、既存Profileを保持する。

## JSON構造

```json
{
  "profileFormatVersion": 1,
  "device": {
    "vid": 13389,
    "pid": 8496,
    "serial": "取得できた場合のみ格納",
    "product": "USB device product name"
  },
  "captureMode": {
    "pixelFormat": "MJPEG",
    "width": 1280,
    "height": 720,
    "frameRate": 60
  },
  "inputContract": {
    "inputEncoding": "RGB",
    "range": "FULL",
    "colorimetry": "BT709",
    "transferCharacteristics": "SDR"
  },
  "pu": {
    "brightness": 0,
    "contrast": 32,
    "saturation": 64,
    "hue": 0
  },
  "correction": {
    "matrix": [[1.0, 0.0, 0.0], [0.0, 1.0, 0.0], [0.0, 0.0, 1.0]],
    "offsetCode": [0.0, 0.0, 0.0]
  },
  "validation": {
    "result": "CALIBRATION_VALID",
    "maeBefore": 0.0,
    "maeAfter": 0.0,
    "rmseBefore": 0.0,
    "rmseAfter": 0.0,
    "maximumChannelError": 0.0,
    "neutralAxisRmse": 0.0,
    "primaryRmse": 0.0,
    "secondaryRmse": 0.0,
    "blackOffsetMax": 0.0,
    "whiteErrorMax": 0.0,
    "clippingDetected": false,
    "matrixCondition": 0.0
  },
  "metadata": {
    "calibrationModelVersion": "offset-matrix-v1",
    "patternVersion": "24patch-v1",
    "solverVersion": "least-squares-v1",
    "transportDecode": "BT.601_LIMITED",
    "timestampUtc": "2026-09-29T00:00:00Z"
  }
}
```

数値は例であり、実際のProfileには実測値を格納する。

## 必須Matching条件

Field Monitorは、少なくとも次の項目が完全一致するProfileだけを適用する。

- `device.vid` / `device.pid`
- `captureMode.pixelFormat`
- `captureMode.width` / `captureMode.height`
- `inputContract.inputEncoding`
- `inputContract.range`
- `inputContract.colorimetry`
- `inputContract.transferCharacteristics`

Serialを取得できる機器では`device.serial`も個体識別に使用する。条件不一致時は補間や推測を行わず、`PROFILE_MODE_MISMATCH`として補正を無効化する。

## 保存と再読込

アプリ内部の`calibration_profiles`ディレクトリへ、Device IdentityとInput Contractを含む名前で保存する。一時ファイルの書込みと`fsync`を行った後に置換し、ファイルシステムが対応する場合はAtomic Moveを使用する。再起動時は同ディレクトリ内のJSONを解析し、読込できたProfileを画面に表示する。

外部受け渡しはAndroidのドキュメント作成UIを使い、ユーザーが選択した場所へ同じJSONをExportする。

### ファイル名

内部保存名にはDevice Identity、Capture Mode、Input Contractを含め、同じ条件の最新Profileを置換する。

```text
UVC_334D_2130_1280x720p60_BT709_SDR_RGB_FULL.json
```

Serialを取得できる場合はVID/PIDの直後へ追加する。外部Exportの候補名には、履歴を識別できるようUTC Timestampも追加する。

```text
UVC_334D_2130_SN1234_1280x720p60_BT709_SDR_RGB_FULL_20260930T173500Z.json
```

ファイル名の色域には`inputContract.colorimetry`、SDR/PQには`inputContract.transferCharacteristics`を使用する。JSON構造およびProfile Matching条件はファイル名に依存しない。
