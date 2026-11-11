# Third-party components

ModelHub releases (`ModelHub-<version>-win64.zip`) include or are built from the components below, unmodified.
llama.cpp is downloaded from its official release and checked against the SHA-256 pinned in `cmake/Llama.cmake`.
The libraries come from vcpkg (`vcpkg.json`).

| Component | Version | Where | Licence | Source |
| --- | --- | --- | --- | --- |
| llama.cpp (`llama-server`, Vulkan build by default) | b11938 | `bin\llama` | MIT | https://github.com/ggml-org/llama.cpp |
| LLVM OpenMP runtime (part of the llama.cpp release) | as shipped | `bin\llama` | Apache License 2.0 with LLVM exceptions (`bin\llama\LICENSE-LLVM-OpenMP`) | https://github.com/llvm/llvm-project |
| cpp-httplib | vcpkg | linked into `ModelHub.exe` | MIT | https://github.com/yhirose/cpp-httplib |
| nlohmann/json | vcpkg | linked into every program and `ModelHubSdk.dll` | MIT | https://github.com/nlohmann/json |
| Microsoft Visual C++ runtime | VS 2022/2026 | `bin\Release\*.dll` | Microsoft redistributable terms | https://visualstudio.microsoft.com |

GoogleTest is used only to build and run the tests and is not shipped.

## Notes

- With `-DMODELHUB_LLAMA_BACKEND=cuda`, NVIDIA's CUDA 12.4 runtime DLLs from the llama.cpp release are also
  shipped in `bin\llama`, under NVIDIA's CUDA redistribution terms.
- Models are not bundled. ModelHub downloads GGUF files from Hugging Face when a user or an application asks for
  them; each model's licence is in the catalog (or on its Hugging Face page) and some do not allow commercial use.

## Licence texts

### llama.cpp

```text
MIT License

Copyright (c) 2023-2026 The ggml authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### cpp-httplib

```text
The MIT License (MIT)

Copyright (c) 2017 yhirose

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### nlohmann/json

```text
MIT License

Copyright (c) 2013-2025 Niels Lohmann

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```
