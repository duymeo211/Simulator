/**
 * @file    sim_log.h
 * @brief   Simulator system log (INFO/WARN/ERROR) -> simulator.log
 * @date    2026-05
 */
#ifndef SIM_LOG_H
#define SIM_LOG_H

#include <stdbool.h>

 /* ------------------------------------------------------------------ */
 /* Public API                                                         */
 /* ------------------------------------------------------------------ */

 /**
  * @brief  Initialize system log file (simulator.log).
  * @param  file_path Output file path (e.g. "simulator.log").
  * @return true on success, false on failure.
  */
bool sim_log_init(const char* file_path);

/**
 * @brief Stop system log (flush + close).
 */
void sim_log_stop(void);

/**
 * @brief Write INFO/WARN/ERROR line (timestamped).
 */
void sim_log_info(const char* fmt, ...);
void sim_log_warn(const char* fmt, ...);
void sim_log_error(const char* fmt, ...);

#endif /* SIM_LOG_H */