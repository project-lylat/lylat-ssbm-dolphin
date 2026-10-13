# Nayuki's QR Code generator library (C, MIT, v1.8.0). Used by the QR sign-in dialog.
add_library(qrcodegen STATIC qrcodegen.c qrcodegen.h)
target_include_directories(qrcodegen PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
dolphin_disable_warnings(qrcodegen)
