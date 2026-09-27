# Автоматическая проверка CUDA и HIP

На Windows запустите из корня проекта одну команду:

```powershell
pwsh -File tools/verify-vendor.ps1
```

Если установлен только Windows PowerShell, используйте `powershell -File tools/verify-vendor.ps1`. Скрипт сам обнаруживает компиляторы CUDA и HIP, берёт архитектуру AMD из `hipInfo`, собирает подходящие модули, определяет GPU и проверяет каждый доступный CUDA/HIP backend на эталонной фразе и коротком поиске. Вводить модель карты, `gfx` или архитектуру CUDA не требуется. Отчёт сохраняется в `build/vendor-validation/vendor-report.txt`; отправьте именно этот файл и вывод сборки, если сборка остановилась до создания отчёта.

На Linux запустите `bash tools/verify-vendor.sh`. Скрипт также сам выбирает доступные модули и устройства и записывает отчёт в `build/vendor-validation/vendor-report.txt`.

Перед запуском нужны совместимые драйвер, SDK соответствующего производителя, C++ toolchain и vcpkg. CUDA и HIP независимы: для проверки NVIDIA не требуется HIP SDK, для AMD не требуется CUDA Toolkit. Если SDK или карта несовместимы, отчёт отмечает `UNAVAILABLE`, приложение продолжает работать через Vulkan или CPU. Проверяйте поддержку конкретной AMD GPU по [матрице HIP SDK](https://rocm.docs.amd.com/projects/install-on-windows/en/latest/reference/system-requirements.html). AMD RX 6650 XT на основной машине VANTIX не входит в матрицу HIP SDK 7.2.0 для Windows.

Windows HIP SDK не поддерживает CMake HIP language, поэтому проект автоматически вызывает `hipcc` для отдельной DLL. На Linux сборка HIP использует CMake HIP language. После проверки сравнивайте только скорость **полной генерации проверенных TON-адресов**, которую печатает `--benchmark`.

CUDA/HIP реализованы в исходниках, но не подтверждены на подходящем оборудовании и не включены в установщики 0.1.0. Я добавлю их в релиз после успешной сборки, проверки эталонных векторов, короткого поиска и сохранения кошелька на поддерживаемой карте.
