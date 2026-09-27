# ============================================================================
# Atoms — ATOM-01 ~ ATOM-19 模块组装清单
#
# 由上层 NewVersion/CMakeLists.txt 通过 add_subdirectory(Atoms) 引入。
# 每个子目录提供统一的 Marshland::atom_NN_<名称> 目标；本文件把它们汇总为
# marshland_atoms 聚合接口目标，供上层工程一次性链接。
#
# 模块清单与 Design/Atoms.md 第 3 节的 19 个功能类一一对应。
#
# 新增模块步骤：
#   1. 新建 Atoms/ATOM-NN_<名称>/ 目录；
#   2. 复制任一子 CMakeLists.txt（19 份均为同一模板），只改 ATOM_MODULE_TARGET；
#   3. 在本文件按序号插入一行 add_subdirectory()，并加入聚合链接列表。
# ============================================================================

# --- 模块声明：顺序与 ATOM 序号一致 -----------------------------------------
add_subdirectory(ATOM-01_Runtime)
add_subdirectory(ATOM-02_State_Arbitration)
add_subdirectory(ATOM-03_SBUS)
add_subdirectory(ATOM-04_CAN)
add_subdirectory(ATOM-05_Motor_PID)
add_subdirectory(ATOM-06_GPIO_Sensors)
add_subdirectory(ATOM-07_Servo_Driver)
add_subdirectory(ATOM-08_Charge)
add_subdirectory(ATOM-09_Force)
add_subdirectory(ATOM-10_Reload_Feed)
add_subdirectory(ATOM-11_Fire)
add_subdirectory(ATOM-12_Vision_YAW)
add_subdirectory(ATOM-13_Auto_Fire)
add_subdirectory(ATOM-14_Safety_Recovery)
add_subdirectory(ATOM-15_Config_Persistence)
add_subdirectory(ATOM-16_HTTP_Web)
add_subdirectory(ATOM-17_WebSocket)
add_subdirectory(ATOM-18_Diagnostics_Logging)
add_subdirectory(ATOM-19_Debug_Calibration)

# --- 聚合目标：上层只需链接 marshland_atoms ---------------------------------
add_library(marshland_atoms INTERFACE)
add_library(Marshland::Atoms ALIAS marshland_atoms)

target_link_libraries(marshland_atoms INTERFACE
    Marshland::atom_01_runtime
    Marshland::atom_02_state_arbitration
    Marshland::atom_03_sbus
    Marshland::atom_04_can
    Marshland::atom_05_motor_pid
    Marshland::atom_06_gpio_sensors
    Marshland::atom_07_servo_driver
    Marshland::atom_08_charge
    Marshland::atom_09_force
    Marshland::atom_10_reload_feed
    Marshland::atom_11_fire
    Marshland::atom_12_vision_yaw
    Marshland::atom_13_auto_fire
    Marshland::atom_14_safety_recovery
    Marshland::atom_15_config_persistence
    Marshland::atom_16_http_web
    Marshland::atom_17_websocket
    Marshland::atom_18_diagnostics_logging
    Marshland::atom_19_debug_calibration
)
