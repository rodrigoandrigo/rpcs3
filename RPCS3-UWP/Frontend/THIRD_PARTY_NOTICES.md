# Third-Party Notices

The Zero-Clause BSD license in `LICENSE.txt` applies only to original,
project-owned code in UWP ImGui Frontend. Third-party software and assets keep
their own licenses.

## Bundled Components

| Component | License | License file |
| --- | --- | --- |
| Dear ImGui | MIT | `dependencies/imgui/LICENSE.txt` |
| Lato Medium font | SIL Open Font License 1.1 | `UWP-ImGuiFrontend/assets/fonts/OFL.txt` |

Redistributions that include these files must comply with their respective
license terms and retain the corresponding notices.

## Build Dependencies

The following dependencies are resolved through vcpkg and are not relicensed
by this project:

| Component | License |
| --- | --- |
| Boost.Asio and Boost headers | Boost Software License 1.0 |
| fmt | MIT |
| RapidJSON | MIT |

The Windows SDK, Visual Studio toolchain, and vcpkg are not distributed as part
of this repository. Consult the exact dependency versions selected by vcpkg for
their complete notices and any transitive-license requirements.
