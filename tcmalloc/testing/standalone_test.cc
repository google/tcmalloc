#include <iostream>
#include <cstddef>

int main() {
  std::cout << "Standard Alignment: " << alignof(std::max_align_t) << '\n';

  double *ptr = (double*) malloc(sizeof(double));
  std::cout << "Double Alignment: " << alignof(*ptr) << '\n';

  char *ptr2 = (char*) malloc(1);
  std::cout << "Char Alignment: " << alignof(*ptr2) << '\n';

  void *ptr3;
  std::cout << "Sizeof void*: " << sizeof(ptr3) << '\n';

  return 0;
}
