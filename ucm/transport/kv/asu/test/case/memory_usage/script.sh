cmake -S . -B build-asu-memory-probe \
  -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_UCM_ASU=ON \
  -DBUILD_UCM_STORE=OFF \
  -DBUILD_UCM_SPARSE=OFF \
  -DBUILD_UNIT_TESTS=ON \
  -DRUNTIME_ENVIRONMENT=ascend \
  -DBUILD_UCM_ASU_PROVIDER_FAKE=ON

cmake --build build-asu-memory-probe --target asu.test -j8

ASU_CLIENT_MEMORY_PROBE_CONFIG=ucm/transport/kv/asu/test/case/asu_client_memory_probe.conf \
./build-asu-memory-probe/ucm/transport/kv/asu/asu.test \
  --gtest_filter=AsuClientMemoryProbe.IdleClientInitialization