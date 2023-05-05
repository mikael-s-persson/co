// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#ifndef coroutine_h
#define coroutine_h

#include "bitset.h"
#include <csetjmp>
#include <cstdint>
#include <ctime>
#include <functional>
#include <list>
#include <poll.h>
#include <string>
#include <vector>

namespace co {

class CoroutineMachine;
class Coroutine;

using CoroutineFunctor = std::function<void(Coroutine *)>;
using CompletionCallback = std::function<void(Coroutine *)>;

constexpr size_t kCoDefaultStackSize = 32 * 1024;

extern "C" {
void __co_Invoke(class Coroutine *c);
}

// This is a Coroutine.  It executes its functor (pointer to a function
// or a lambda).
//
// It has its own stack with default size kCoDefaultStackSize.
// By default, the coroutine will be given a unique name and will
// be started automatically.  It can have some user data which is
// not owned by the coroutine.
class Coroutine {
public:
  Coroutine(CoroutineMachine &machine, CoroutineFunctor functor,
            const char *name = nullptr, bool autostart = true,
            size_t stack_size = kCoDefaultStackSize, void *user_data = nullptr);

  ~Coroutine();

  // Start a coroutine running if it is not already running,
  void Start();

  // Yield control to another coroutine.
  void Yield();

  // Call another coroutine and store the result.
  template <typename T>
  T Call(Coroutine &callee);

  // Yield control and store value.
  template <typename T>
  void YieldValue(const T& value);

  // For all Wait functions, the timeout is optional and if greater than zero
  // specifies a nanosecond timeout.  If the timeout occurs before the fd (or
  // one of the fds) becomes ready, Wait will return -1. If an fd is ready, Wait
  // will return the fd that terminated the wait.

  // Wait for a file descriptor to become ready.  Returns the fd if it
  // was triggered or -1 for timeout.
  int Wait(int fd, short event_mask = POLLIN, int64_t timeout_ns = 0);

  // Wait for a pollfd.   Returns the fd if it was triggered or -1 for timeout.
  int Wait(struct pollfd &fd, int64_t timeout_ns = 0);

  // Wait for a set of pollfds.  Each needs to specify an fd and an event.
  // Returns the fd that was triggered, or -1 for a timeout.
  int Wait(const std::vector<struct pollfd> &fds, int64_t timeout_ns = 0);

  void Exit();

  // Sleeping functions.
  void Nanosleep(uint64_t ns);
  void Millisleep(time_t msecs) { Nanosleep(msecs * 1000000LL); }
  void Sleep(time_t secs) { Nanosleep(secs * 1000000000LL); }

  // Set and get the name.  You can change the name at any time.  It's
  // only for debug really.
  void SetName(const std::string &name) { name_ = name; }
  const std::string &Name() const { return name_; }

  // Set and get the user data (not owned by the coroutine).  It's up
  // to you what this contains and you are responsible for its
  // management.
  void SetUserData(void *user_data) { user_data_ = user_data; }
  void *UserData() const { return user_data_; }

  // Is the given coroutine alive?
  bool IsAlive();

  uint64_t LastTick() const { return last_tick_; }
  CoroutineMachine &Machine() const { return machine_; }

  void Show();

  // Each coroutine has a unique id.
  int64_t Id() const { return id_; }

private:
  enum class State {
    kCoNew,
    kCoReady,
    kCoRunning,
    kCoYielded,
    kCoWaiting,
    kCoDead,
  };
  friend class CoroutineMachine;
  friend void __co_Invoke(Coroutine *c);
  void InvokeFunctor();
  int EndOfWait(int timer_fd, int result);
  int AddTimeout(int64_t timeout_ns);
  State GetState() const { return state_; }
  void AddPollFds(std::vector<struct pollfd> &pollfds,
                  std::vector<Coroutine *> &covec);
  void Resume(int value);
  void TriggerEvent();
  void ClearEvent();

  CoroutineMachine &machine_;
  size_t id_;                // Coroutine ID.
  CoroutineFunctor functor_; // Coroutine body.
  std::string name_;         // Optional name.
  State state_;
  void *stack_;                     // Stack, allocated from malloc.
  void *yielded_address_ = nullptr; // Address at which we've yielded.
  size_t stack_size_;
  jmp_buf resume_;                      // Program environemnt for resuming.
  jmp_buf exit_;                        // Program environemt to exit.
  struct pollfd event_fd_;              // Pollfd for event.
  std::vector<struct pollfd> wait_fds_; // Pollfds for waiting for an fd.
  Coroutine *caller_ = nullptr;         // If being called, who is calling us.
  void *result_ = nullptr;              // Where to put result in YieldValue.
  void *user_data_;                     // User data, not owned by this.
  uint64_t last_tick_ = 0;              // Tick count of last resume.
};

struct PollState {
  std::vector<struct pollfd> pollfds;
  std::vector<Coroutine *> coroutines;
};

class CoroutineMachine {
public:
  CoroutineMachine();
  ~CoroutineMachine();

  // Run the machine until all coroutines have terminated or
  // told to stop.
  void Run();

  // Stop the machine.  Running coroutines will not be terminated.
  void Stop();

  void AddCoroutine(Coroutine *c);
  void RemoveCoroutine(Coroutine *c);
  void StartCoroutine(Coroutine *c);

  // When you don't want to use the Run function, these
  // functions allow you to incorporate the multiplexed
  // IO into your own poll loop.
  void GetPollState(PollState *poll_state);
  void ProcessPoll(PollState *poll_state);

  // Print the state of all the coroutines to stderr.
  void Show();

  // Call the given function when a coroutine exits. 
  // You can use this to delete the coroutine.
  void SetCompletionCallback(CompletionCallback callback) {
    completion_callback_ = callback;
  }

private:
  friend class Coroutine;
  struct ChosenCoroutine {
    ChosenCoroutine() = default;
    ChosenCoroutine(Coroutine *c, int f) : co(c), fd(f) {}
    Coroutine *co = nullptr;
    int fd = 0x12345678;
  };
  
  void BuildPollFds(PollState *poll_state);
  ChosenCoroutine ChooseRunnable(PollState *poll_state, int num_ready);

  ChosenCoroutine GetRunnableCoroutine(PollState *poll_state, int num_ready);
  size_t AllocateId();
  uint64_t TickCount() const { return tick_count_; }
  bool IdExists(int id) const { return coroutine_ids_.Contains(id); }
  jmp_buf &YieldBuf() { return yield_; }

  std::list<Coroutine *> coroutines_;
  BitSet coroutine_ids_;
  ssize_t last_freed_coroutine_id_ = -1;
  std::vector<ChosenCoroutine> ready_coroutines_;
  jmp_buf yield_;
  bool running_ = false;
  PollState poll_state_;
  struct pollfd interrupt_fd_;
  uint64_t tick_count_ = 0;
  CompletionCallback completion_callback_;
};

template <typename T>
inline void Coroutine::YieldValue(const T& value) {
  // Copy value.
  if (result_ != nullptr) {
    memcpy(result_, &value, sizeof(value));
  }
  if (caller_ != nullptr) {
    // Tell caller that there's a value available.
    caller_->TriggerEvent();
  }

  // Yield control to another coroutine but don't trigger a wakup event.
  // This will be done when another call is made.
  state_ = State::kCoYielded;
  last_tick_ = machine_.TickCount();
  if (setjmp(resume_) == 0) {
    longjmp(machine_.YieldBuf(), 1);
    // Never get here.
  }
  // We get here when resumed from another call.
}

template <typename T>
inline T Coroutine::Call(Coroutine &callee) {
  T result;
  // Tell the callee that it's being called and where to store the value.
  callee.caller_ = this;
  callee.result_ = &result;

  // Start the callee running if it's not already running.  If it's running
  // we trigger its event to wake it up.
  if (callee.state_ == State::kCoNew) {
    callee.Start();
  } else {
    callee.TriggerEvent();
  }
  state_ = State::kCoYielded;
  last_tick_ = machine_.TickCount();
  if (setjmp(resume_) == 0) {
    longjmp(machine_.YieldBuf(), 1);
    // Never get here.
  }
  // When we get here, the callee has done its work.  Remove this coroutine's
  // state from it.
  callee.caller_ = nullptr;
  callee.result_ = nullptr;
  return result;
}

} // namespace co
#endif /* coroutine_h */
