# Vendored libjpeg-turbo

This directory contains the prebuilt arm64-v8a TurboJPEG library and public
header used by Android UVC Calibrator.

- Upstream: https://github.com/libjpeg-turbo/libjpeg-turbo
- Version: `3.2.0` (`TURBOJPEG_VERSION_NUMBER` 3002000)
- Licenses: IJG License and Modified BSD (3-clause) License
- Build integration: imported static target `turbojpeg` in the app CMake build

The public header retains its upstream copyright and license notice. Binary
redistribution notices and the Modified BSD license text are retained in the
repository root [`THIRD_PARTY_NOTICES.md`](../../../../../../THIRD_PARTY_NOTICES.md).

When replacing the archive, update the header, archive, version above, and
third-party notices together. Only use an archive built for the matching ABI.
