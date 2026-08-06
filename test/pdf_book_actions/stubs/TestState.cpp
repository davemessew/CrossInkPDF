#include "TestState.h"

#include <cstring>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "HalStorage.h"
#include "I18n.h"
#include "RecentBooksStore.h"

BookActionTestState TEST_STATE;
FakeI18n I18N;
#ifndef PDF_BOOK_ACTIONS_PRODUCTION_STATS
FakeStorage Storage;
#endif
FakeCrossPointSettings TEST_SETTINGS;
FakeCrossPointState TEST_APP_STATE;
FakeRecentBooksStore TEST_RECENT_BOOKS;

void resetBookActionTestState() {
  TEST_STATE = {};
  TEST_SETTINGS = {};
  TEST_APP_STATE.hasPendingAlert.store(false);
  TEST_APP_STATE.pendingAlertGoHomeOnBack.store(false);
  std::memset(TEST_APP_STATE.pendingAlertTitle, 0, sizeof(TEST_APP_STATE.pendingAlertTitle));
  std::memset(TEST_APP_STATE.pendingAlertBody, 0, sizeof(TEST_APP_STATE.pendingAlertBody));
}
