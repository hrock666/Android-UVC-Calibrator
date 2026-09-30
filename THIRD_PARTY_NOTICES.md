# Third-party notices

The MIT License in the repository root applies to the Android UVC Calibrator
project, except for the third-party components listed below. Those components
remain subject to their own licenses.

## libusb 1.0.30

- Upstream: <https://github.com/libusb/libusb>
- License: LGPL-2.1-or-later
- Location: `app/src/main/cpp/third_party/libusb`
- Local license text: [`COPYING`](app/src/main/cpp/third_party/libusb/COPYING)
- Vendoring details: [`VENDORED.md`](app/src/main/cpp/third_party/libusb/VENDORED.md)

The repository contains only the Android/Linux backend subset required by the
current native build. Original source-file notices are retained.

## libjpeg-turbo 3.2.0

- Upstream: <https://github.com/libjpeg-turbo/libjpeg-turbo>
- Licenses: IJG License and Modified BSD (3-clause) License
- Location: `app/src/main/cpp/third_party/libjpeg-turbo`
- Included artifacts: the arm64-v8a TurboJPEG static library and public header

This software is based in part on the work of the Independent JPEG Group.

### Modified BSD (3-clause) License

Copyright (C) 2009-2026 D. R. Commander  
Copyright (C) 2018-2023 Randy <randy408@protonmail.com>

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.
3. Neither the name of the libjpeg-turbo Project nor the names of its
   contributors may be used to endorse or promote products derived from this
   software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

The authoritative license description is available in the upstream
[`LICENSE.md`](https://github.com/libjpeg-turbo/libjpeg-turbo/blob/main/LICENSE.md).
