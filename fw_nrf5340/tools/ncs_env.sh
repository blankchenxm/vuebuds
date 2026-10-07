# Source in Git Bash: sets up the NCS v3.2.1 toolchain installed under D:\NRFSDK.
TC=/d/NRFSDK/toolchains/66cdf9b75e
export ZEPHYR_BASE=/d/NRFSDK/v3.2.1/zephyr
export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
export ZEPHYR_SDK_INSTALL_DIR=$TC/opt/zephyr-sdk
export NRFUTIL_HOME=$TC/nrfutil/home
export PYTHONPATH="$TC/opt/bin;$TC/opt/bin/Lib;$TC/opt/bin/Lib/site-packages"
export PATH=$TC:$TC/mingw64/bin:$TC/bin:$TC/opt/bin:$TC/opt/bin/Scripts:$TC/opt/nanopb/generator-bin:$TC/nrfutil/bin:$TC/opt/zephyr-sdk/arm-zephyr-eabi/bin:$PATH
