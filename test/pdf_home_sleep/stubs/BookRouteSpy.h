#pragma once

struct BookRouteSpyState {
  int epubLoads = 0;
  int epubProgressLoads = 0;
  int epubCoverGenerations = 0;
  int epubThumbGenerations = 0;
  int xtcLoads = 0;
  int xtcCoverGenerations = 0;
  int xtcThumbGenerations = 0;
  int txtLoads = 0;
  int txtCoverGenerations = 0;
  bool epubLoadResult = true;
  bool epubProgressResult = true;
  bool xtcLoadResult = true;
  bool txtLoadResult = true;
  float epubCalculatedProgress = 0.42f;
  float xtcCalculatedProgress = 37.0f;

  void reset() { *this = {}; }
};

inline BookRouteSpyState BOOK_ROUTE_SPY;
