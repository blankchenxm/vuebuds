# Source in a shell to run west by hand with NCS v3.2.1 (tools/dk5340.py does this by itself).
# macOS / Linux: the nrfutil sdk-manager install (/opt/nordic/ncs on macOS).
# Git Bash on Windows: the install under D:\NRFSDK (toolchain 66cdf9b75e).
if [ -z "$MSYSTEM" ] && command -v nrfutil >/dev/null 2>&1 && \
   nrfutil sdk-manager toolchain env --ncs-version v3.2.1 >/dev/null 2>&1; then
  eval "$(nrfutil sdk-manager toolchain env --as-script sh --ncs-version v3.2.1)"
  for d in /opt/nordic/ncs/v3.2.1 "$HOME/ncs/v3.2.1"; do
    [ -d "$d/zephyr" ] && export ZEPHYR_BASE="$d/zephyr" && break
  done
else
  TC=/d/NRFSDK/toolchains/66cdf9b75e
  export ZEPHYR_BASE=/d/NRFSDK/v3.2.1/zephyr
  export ZEPHYR_TOOLCHAIN_VARIANT=zephyr
  export ZEPHYR_SDK_INSTALL_DIR=$TC/opt/zephyr-sdk
  export NRFUTIL_HOME=$TC/nrfutil/home
  export PYTHONPATH="$TC/opt/bin;$TC/opt/bin/Lib;$TC/opt/bin/Lib/site-packages"
  export PATH=$TC:$TC/mingw64/bin:$TC/bin:$TC/opt/bin:$TC/opt/bin/Scripts:$TC/opt/nanopb/generator-bin:$TC/nrfutil/bin:$TC/opt/zephyr-sdk/arm-zephyr-eabi/bin:$PATH
fi
