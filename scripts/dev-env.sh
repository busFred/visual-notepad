# Source this file (do not execute it) to use the project-local toolchain:
#   source scripts/dev-env.sh
# Expects the conda env at .toolchain/env and KDE libraries at .toolchain/kf6.

_vn_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
_vn_tc="$_vn_root/.toolchain"

if [ ! -d "$_vn_tc/env" ]; then
    echo "dev-env: $_vn_tc/env not found" >&2
    return 1
fi

_vn_conda="${CONDA_EXE:-$HOME/miniconda3/bin/conda}"
# shellcheck disable=SC1091
source "$(dirname "$_vn_conda")/../etc/profile.d/conda.sh"
conda activate "$_vn_tc/env"

export CARGO_HOME="$_vn_tc/cargo"
export CMAKE_PREFIX_PATH="$_vn_tc/kf6;$CONDA_PREFIX"
export PATH="$CARGO_HOME/bin:$PATH"

unset _vn_root _vn_tc _vn_conda
