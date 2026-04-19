cmake_minimum_required(VERSION 3.20)
project(task_scheduler_runtime CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(BASE_FLAGS
    -Wall -Wextra -Wpedantic
    -Wno-unused-parameter
    -march=native
    -fno-omit-frame-pointer
)

set(RELEASE_FLAGS -O3 -DNDEBUG)
set(ASAN_FLAGS    -fsanitize=address,undefined -fno-sanitize-recover=undefined -g -O1)
set(TSAN_FLAGS    -fsanitize=thread -g -O1)

include_directories(${CMAKE_SOURCE_DIR}/include)

# --- Release benchmark: original throughput suite
add_executable(bench_runner bench/main.cpp)
target_compile_options(bench_runner PRIVATE ${BASE_FLAGS} ${RELEASE_FLAGS})
target_link_libraries(bench_runner PRIVATE pthread)

# --- Release benchmark: scaling sweep (1/2/4/8 threads)
add_executable(bench_scaling bench/scaling.cpp)
target_compile_options(bench_scaling PRIVATE ${BASE_FLAGS} ${RELEASE_FLAGS})
target_link_libraries(bench_scaling PRIVATE pthread)

# --- Unit tests (no sanitizers)
add_executable(test_runner src/tests.cpp)
target_compile_options(test_runner PRIVATE ${BASE_FLAGS} -O2 -g)
target_link_libraries(test_runner PRIVATE pthread)

# --- ASan + UBSan tests
add_executable(test_asan src/tests.cpp)
target_compile_options(test_asan PRIVATE ${BASE_FLAGS} ${ASAN_FLAGS})
target_link_options(test_asan PRIVATE ${ASAN_FLAGS})
target_link_libraries(test_asan PRIVATE pthread)

# --- TSan tests
add_executable(test_tsan src/tests.cpp)
target_compile_options(test_tsan PRIVATE ${BASE_FLAGS} ${TSAN_FLAGS})
target_link_options(test_tsan PRIVATE ${TSAN_FLAGS})
target_link_libraries(test_tsan PRIVATE pthread)

enable_testing()
add_test(NAME unit_tests COMMAND test_runner)
add_test(NAME asan_tests COMMAND test_asan)
add_test(NAME tsan_tests COMMAND test_tsan)
