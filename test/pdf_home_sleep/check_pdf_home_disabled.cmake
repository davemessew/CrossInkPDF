if(NOT EXISTS "${SLEEP_OBJECT}")
  message(FATAL_ERROR "disabled PDF sleep object is missing: ${SLEEP_OBJECT}")
endif()

file(SIZE "${SLEEP_OBJECT}" sleep_object_size)
if(sleep_object_size GREATER 4096)
  message(FATAL_ERROR "disabled PDF sleep object is ${sleep_object_size} bytes; expected <= 4096")
endif()

execute_process(
  COMMAND "${NM}" -C "${SLEEP_OBJECT}"
  RESULT_VARIABLE nm_result
  OUTPUT_VARIABLE nm_output
  ERROR_VARIABLE nm_error
)
if(NOT nm_result EQUAL 0)
  message(FATAL_ERROR "nm failed for disabled PDF sleep object: ${nm_error}")
endif()
if(nm_output MATCHES "PdfSleep|pdfSnapshotBeforeFallback|capturePdfSleep")
  message(FATAL_ERROR "disabled PDF sleep object retains PDF symbols or references:\n${nm_output}")
endif()

if(NOT EXISTS "${HOME_OBJECT}")
  message(FATAL_ERROR "disabled PDF Home/recent object is missing: ${HOME_OBJECT}")
endif()

execute_process(
  COMMAND "${NM}" -C "${HOME_OBJECT}"
  RESULT_VARIABLE nm_result
  OUTPUT_VARIABLE nm_output
  ERROR_VARIABLE nm_error
)
if(NOT nm_result EQUAL 0)
  message(FATAL_ERROR "nm failed for disabled PDF Home/recent object: ${nm_error}")
endif()

if(nm_output MATCHES
   "PdfProductCache|hydratePdfBook|pdfPathHash|pdfFormatCacheRoot|pdfLoadCachedProductState|hasPdfExtension")
  message(FATAL_ERROR "disabled PDF Home/recent object retains PDF symbols or references:\n${nm_output}")
endif()
