#pragma once

#include "motor_types.hpp"

namespace marshland::atom05 {

// C620: angle[0:2], signed rpm[2:4], current[4:6], temp[6], reserved[7].
// Do not infer error codes from the reserved byte or integrate position.
MotorFault DecodeMotorFeedback(const can::CanFrame& frame,
                                MotorFeedback& output) noexcept;

} // namespace marshland::atom05
