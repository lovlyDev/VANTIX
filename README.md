# VANTIX

Я разрабатываю VANTIX — генератор TON-адресов с заданным фрагментом. Вычислительное ядро написано на C++23, приложение для Windows и Linux — на Tauri и React. В выпуске 0.1.0 доступны CPU и Vulkan Compute. Исходники CUDA/HIP есть в проекте, но их модули не входят в установщики: сборка и работа на поддерживаемых видеокартах ещё не проверены.

## Скачать и установить

На странице [выпуска 0.1.0](https://github.com/lovlyDev/VANTIX/releases/tag/v0.1.0) доступны:

- VANTIX_0.1.0_x64-setup.exe — установщик Windows;
- VANTIX_0.1.0_amd64.deb — пакет Linux;
- VANTIX_0.1.0_amd64.AppImage — переносимый пакет Linux;
- SHA256SUMS.txt — контрольные суммы.

При запуске приложение обнаруживает устройства и проверяет вычислительные режимы. Режим Auto выбирает самую быструю проверенную конфигурацию. Движок и GPU можно выбрать вручную.

## Возможности

- Кошельки TON V5R1 и V4R2; фразы восстановления на 12 или 24 слова.
- Поиск фрагмента в начале, конце или любой части адреса, в том числе без учёта регистра.
- Параллельный поиск на CPU, Vulkan Compute для 12-словной схемы и гибридный режим CPU/GPU. Результат GPU повторно проверяется на CPU.
- Автоматическая калибровка и кэш профиля для оборудования и драйвера.
- Пауза и остановка поиска, график скорости, выбор устройства и нагрузки GPU.
- Шифрование найденной фразы: DPAPI на Windows, AES-256-GCM с паролем на Linux.
- Подписанные обновления приложения для Windows и Linux AppImage; DEB обновляется через менеджер пакетов.

Файл data/ton-english.txt содержит стандартный список слов для фразы восстановления. Он **не ограничивает** фрагмент, который можно искать в адресе. Адрес допускает латинские буквы, цифры, «-» и «_».

## Ограничения выпуска

- CUDA и HIP реализованы в исходниках, но не собраны и не проверены на поддерживаемых видеокартах; их модулей нет в установщиках 0.1.0.
- Работа нескольких физических GPU вместе пока не подтверждена аппаратной проверкой.
- Для 24-словной схемы используется CPU.
- Windows установщик не подписан сертификатом издателя; SmartScreen может показать предупреждение.

Подробности: [ТЗ для AMD и NVIDIA](docs/gpu-backends.md), [проверка CUDA/HIP](docs/vendor-validation.md), [процесс выпуска](docs/release.md).

## Сборка из исходников

Нужны компилятор C++23, CMake 3.24+, vcpkg, Node.js и Rust для приложения. Манифест vcpkg устанавливает OpenSSL 3, заголовки Vulkan, Volk, CLI11 и toml++. CUDA Toolkit и HIP SDK необязательны для CPU/Vulkan; при их наличии CMake пытается собрать независимые модули производителя.

Windows:

~~~powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
~~~

Linux:

~~~bash
cmake -S . -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build
ctest --test-dir build --output-on-failure
~~~

Для автоматической проверки CUDA/HIP на совместимом компьютере предусмотрены tools/verify-vendor.ps1 и tools/verify-vendor.sh. Скрипты определяют оборудование и создают отчёт без ручного ввода архитектуры GPU.

## Командная строка

~~~text
vantix --devices
vantix --backends-json
vantix --self-test
vantix --benchmark
vantix --contains lucky --ignore-case --mnemonic 12 --backend auto
vantix --prefix UQabc --wallet v5 --max 100000 --out matches
vantix --suffix xyz --wallet v4 --batch 16
vantix --contains ton --backend hybrid --progress
~~~

Совпадение проверяется по полному дружественному адресу основной сети. Без --max поиск продолжается до совпадения или остановки. Ключ --device ограничивает поиск выбранным GPU, а --backend задаёт режим вручную. CUDA/HIP появляются как варианты только после проверки их модулей на устройстве.

Фраза восстановления даёт контроль над кошельком. На Linux приложение запрашивает пароль длиной от 12 символов; CLI читает его из переменной VANTIX_WALLET_PASSWORD. Пароль не сохраняется приложением, и при его потере расшифровать результат нельзя. Команда --reveal перед показом фразы проверяет сохранённый адрес. Тестовые фразы из исходников нельзя использовать для средств.

## Источники по TON

- [Описание кошельков TON](https://github.com/ton-blockchain/TEPs/blob/master/text/0003-wallets.md)
- [Эталонная реализация мнемоники](https://github.com/ton-org/ton-crypto/blob/master/src/mnemonic/mnemonic.ts)
- [Контракт Wallet V4R2](https://github.com/ton-org/ton/blob/master/src/wallets/v4/WalletContractV4.ts)
- [Контракт Wallet V5R1](https://github.com/ton-org/ton/blob/master/src/wallets/v5r1/WalletContractV5R1.ts)
- [Стандартный список слов](https://github.com/ton-org/ton-crypto/blob/master/src/mnemonic/wordlist.ts)
