#pragma once

class HalSpiBus {
 public:
  class Lock {
   public:
    Lock();
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

   private:
    bool acquired = false;
  };

  static HalSpiBus& getInstance();

 private:
  HalSpiBus();

  friend class Lock;
};
