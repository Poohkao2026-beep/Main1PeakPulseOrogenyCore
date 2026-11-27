#include <peak_pulse/ring_buffer.hpp>

int main() {
  peak_pulse::MpmcRingBuffer<int, 2> queue;
  if (queue.try_pop() || !queue.try_push(41) || !queue.try_push(42) ||
      queue.try_push(43)) {
    return 1;
  }

  const auto first = queue.try_pop();
  if (first != 41 || !queue.try_push(43)) {
    return 1;
  }

  const auto second = queue.try_pop();
  const auto third = queue.try_pop();
  return second == 42 && third == 43 && !queue.try_pop() ? 0 : 1;
}
