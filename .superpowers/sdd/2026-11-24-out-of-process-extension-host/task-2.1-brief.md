### Task 2.1: PDU vocabulary and codec (`remote/proto/`), channels and framed streams (`remote/transport/`)

**Files:**
- Create: `src/vtextension/wasm/remote/proto/{Wire.hpp,Pdu.hpp,PduTable.hpp,Codec.hpp,Codec.cpp,Codec_test.cpp,Codec_fuzz.cpp}`
- Create: `src/vtextension/wasm/remote/transport/{Channel.hpp,Channel_posix.cpp,Channel_win32.cpp,PduStream.hpp,PduStream.cpp,Channel_test.cpp,PduStream_test.cpp}`
- Create: `src/vtextension/wasm/remote/CMakeLists.txt` (target `vtextension_remote_wire`: proto + transport, links only `ws2_32` on Windows; option `VTEXTENSION_FUZZING` OFF → `vtextension_remote_fuzz` with `-fsanitize=fuzzer`, Clang only)
- Modify: `src/vtextension/CMakeLists.txt` (`add_subdirectory(wasm/remote)` before the backend; the three `_test.cpp` join `vtextension_test`, which links `vtextension_remote_wire`)
- Modify: `src/vtextension/wasm/WasmBinary.hpp` (`ImportSectionId = 2`, `declaredCounts()`)

**Interfaces:**
- Produces:
  ```cpp
  namespace vtextension::wasm::remote::proto {
  inline constexpr uint32_t ProtocolVersion = 1;
  inline constexpr std::size_t MaxCallValues = 64;
  inline constexpr std::size_t MaxHostParameters = 17;      // == wasmtime::MaxHostParameters (static_assert there)
  enum class MemoryMode : uint8_t { Copy, Shared };
  enum class LockdownState : uint8_t { Failed, Unsandboxed, LockedDown };
  enum class Direction : uint8_t { ToHelper, ToParent };
  enum class ChannelKind : uint8_t { Control, Lane };
  enum class ReleaseTarget : uint8_t { Instance, Module, Runtime };
  enum class PduKind : uint8_t { Hello = 1, CreateRuntime, RuntimeCreated, Load, Loaded, Instantiate, Instantiated,
      Release, Call, CallHost, HostResult, CallResult, MemoryRead, MemoryData, MemoryWrite };
  enum class MemoryStatus : uint8_t { OutOfBounds, Ok };
  struct PduLimits { uint64_t maxModuleBytes = 16 MiB; uint64_t maxMemoryBytes = 32 MiB; };
  struct WireHostFunction { std::string module; std::string name; uint32_t parameterCount = 0; };
  struct Hello { uint32_t version; MemoryMode memoryMode; LockdownState lockdown; };
  struct CreateRuntime { uint32_t runtimeId; uint64_t memoryBytes; uint64_t stackBytes; std::vector<WireHostFunction> functions; };
  struct RuntimeCreated { uint32_t runtimeId; std::optional<ExtensionError> error; };
  struct Load { uint32_t runtimeId; uint32_t moduleId; std::vector<std::byte> bytes; };
  struct Loaded { uint32_t moduleId; std::optional<ExtensionError> error;
                  std::vector<ImportDescription> imports; std::vector<ExportDescription> exports; };
  struct Instantiate { uint32_t moduleId; uint32_t instanceId; };
  struct Instantiated { uint32_t instanceId; std::optional<ExtensionError> error; std::vector<uint32_t> callableExports; };
  struct Release { ReleaseTarget target; uint32_t id; };
  struct Call { uint32_t instanceId; uint32_t exportOrdinal; std::vector<uint32_t> argv; };
  struct CallHost { uint32_t functionIndex; std::vector<uint32_t> arguments; };
  struct HostResult { std::optional<ExtensionError> error; };
  struct CallResult { std::optional<CallFailure> failure; std::vector<uint32_t> argv; };
  struct MemoryRead { uint32_t instanceId; uint32_t address; uint32_t size; };
  struct MemoryData { MemoryStatus status; std::vector<std::byte> bytes; };
  struct MemoryWrite { uint32_t instanceId; uint32_t address; std::vector<std::byte> bytes; };
  using Pdu = std::variant<Hello, CreateRuntime, RuntimeCreated, Load, Loaded, Instantiate, Instantiated, Release,
                           Call, CallHost, HostResult, CallResult, MemoryRead, MemoryData, MemoryWrite>;
  enum class DecodeError : uint8_t { NeedMoreData, MalformedVarint, OverCap, UnknownKind, WrongDirection,
                                     WrongChannel, Truncated, TrailingBytes, BadEnum };
  struct PduRow { PduKind kind; ChannelKind channel; Direction direction;
                  uint64_t (*sizeCap)(PduLimits const&);
                  std::expected<Pdu, DecodeError> (*decode)(std::span<uint8_t const> body); };
  extern std::span<PduRow const> const PduTable;            // one row per PduKind, in kind order
  [[nodiscard]] PduKind kindOf(Pdu const& pdu) noexcept;
  [[nodiscard]] std::vector<uint8_t> encodeFrame(Pdu const& pdu);
  struct DecodedFrame { Pdu pdu; std::size_t consumed; };
  [[nodiscard]] std::expected<DecodedFrame, DecodeError> decodeFrame(std::span<uint8_t const> buffer,
      Direction arriving, ChannelKind channel, PduLimits const& limits);
  }
  namespace vtextension::wasm::remote::transport {
  enum class ChannelError : uint8_t { Closed, Io };
  class Channel;   // move-only; send(span) / receiveSome(span) -> expected<size_t>; shutdown() noexcept, thread-safe
  struct ChannelPair { Channel parent; Channel helper; };
  [[nodiscard]] std::expected<ChannelPair, ChannelError> makeChannelPair();
  enum class StreamError : uint8_t { Closed, Io, Malformed, OverCap };
  class PduStream {  // owns a Channel and a receive buffer; one reader+writer thread
  public:
      PduStream(Channel channel, ChannelKind kind, Direction arriving, PduLimits limits);
      [[nodiscard]] std::expected<void, StreamError> send(Pdu const& pdu);
      [[nodiscard]] std::expected<Pdu, StreamError> receive();
      void shutdown() noexcept;
  };
  }
  namespace vtextension::wasm::binary {
  struct DeclaredCounts { uint32_t imports = 0; uint32_t exports = 0; };
  [[nodiscard]] std::expected<DeclaredCounts, BinaryError> declaredCounts(std::span<std::byte const> module);
  }
  ```
  The table (body sizes in bytes; M = `maxModuleBytes`, Mem = `maxMemoryBytes`):

  | Kind | Channel | Direction | Cap |
  |---|---|---|---|
  | Hello | Control | ToParent | 16 |
  | CreateRuntime | Control | ToHelper | 64 KiB |
  | RuntimeCreated | Control | ToParent | 16 |
  | Load | Control | ToHelper | M + 64 |
  | Loaded | Control | ToParent | 4 × M |
  | Instantiate | Control | ToHelper | 32 |
  | Instantiated | Control | ToParent | M |
  | Release | Control | ToHelper | 16 |
  | Call | Lane | ToHelper | 16 + 5 × MaxCallValues |
  | CallHost | Lane | ToParent | 16 + 5 × MaxHostParameters |
  | HostResult | Lane | ToHelper | 8 |
  | CallResult | Lane | ToParent | 16 + 5 × MaxCallValues |
  | MemoryRead | Lane | ToHelper | 32 |
  | MemoryData | Lane | ToParent | Mem + 16 |
  | MemoryWrite | Lane | ToHelper | Mem + 32 |

  Encoding: varints for u32/u64, one byte per enum, status `0` = ok / `value + 1`, byte strings and vectors as varint count + elements, reserves through a `boundedReserveCount<T>` like `vthost/proto/Wire.hpp`. Frame: `varint length` (kind + body), `varint kind`, body (D7).

- [ ] **Step 1: Write the codec tests.** In `Codec_test.cpp`:
  - `"PduCodec.tableHasOneRowPerKind"`: `PduTable.size() == 15`, `PduTable[i].kind == PduKind(i + 1)`.
  - `"PduCodec.roundTrip"`: one `DYNAMIC_SECTION` per kind with a populated sample; `decodeFrame(encodeFrame(p), row.direction, row.channel, {})->pdu == p` and `consumed == frame.size()`.
  - `"PduCodec.everyPrefixNeedsMoreData"`: each sample's frame truncated at every `n < size` → `NeedMoreData`, never another error, never a value.
  - `"PduCodec.overCapIsRejectedFromTheHeader"`: per kind, a header declaring `cap + 1` body bytes and no body → `OverCap` (not `NeedMoreData`).
  - `"PduCodec.lyingBodyIsMalformed"`: inner count exceeding the remainder → `Truncated`; one extra byte → `TrailingBytes`; enum byte out of range → `BadEnum`; kind `0` and `16` → `UnknownKind`.
  - `"PduCodec.directionAndChannelAreChecked"`: `Call` as `ToParent` → `WrongDirection`; `Load` on `Lane` → `WrongChannel`.
  - `"PduCodec.mutatedFramesNeverCrash"`: 10 000 frames from `std::mt19937 { 0x5eed }` (byte flips, truncations, splices); each decode returns a value or an error.
  - In `WasmBinary_test.cpp`: `"WasmBinary.declaredCounts"` on `DescribedModule` → `{ .imports = 2, .exports = N }`.
- [ ] **Step 2: Write the transport tests.** `Channel_test.cpp`: `"Channel.pairCarriesBytesBothWays"`, `"Channel.shutdownUnblocksReceive"` (a jthread blocked in `receiveSome` returns `Closed` after `shutdown()`), `"Channel.sendToClosedPeerFails"` (POSIX: SIGPIPE disposition unchanged, send → `Closed`), `"Channel.endsAreCloseOnExec"` (POSIX, `FD_CLOEXEC`). `PduStream_test.cpp`: `"PduStream.roundTripsOverAPair"`, `"PduStream.overCapFailsWithoutWaitingForTheBody"` (header only → `OverCap` at once), `"PduStream.peerCloseIsClosed"`, `"PduStream.garbageIsMalformed"`.
- [ ] **Step 3: Build; expect compile failure.** `nice -n 19 ionice -c 3 cmake --build --preset clang-asan -j 4 --target vtextension_test`.
- [ ] **Step 4: Implement the codec.** `Wire.hpp`: standalone `Writer`/`Reader`, LEB128 (max 5 bytes u32, 10 bytes u64 → `MalformedVarint`) (D3). `Codec.cpp`: one encode and one decode function per kind; `PduTable` a `constexpr std::array` with a static_assert on row order; each body ends with `reader.atEnd()` or `TrailingBytes` (D4). `declaredCounts` reads the import/export section vector counts, 0 when absent.
- [ ] **Step 5: Implement the transport.** POSIX: `socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC)` (macOS: + `fcntl(FD_CLOEXEC)` + `SO_NOSIGPIPE`); `send` with `MSG_NOSIGNAL`, looping on partial writes and `EINTR`; `shutdown()` = `::shutdown(fd, SHUT_RDWR)`; close only in the destructor. Windows: `CreateNamedPipeW(L"\\\\.\\pipe\\contour-extension-host-<pid>-<serial>-<random>", PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, …)` + `CreateFileW(…, FILE_FLAG_OVERLAPPED)`; overlapped I/O waited on; `shutdown()` sets a flag and `CancelIoEx`. `PduStream::receive` grows its buffer until `decodeFrame` stops returning `NeedMoreData`; other decode errors map to `Malformed`/`OverCap`.
- [ ] **Step 6: Add the fuzz target.** `Codec_fuzz.cpp` defines `LLVMFuzzerTestOneInput`, decoding the input as each (direction, channel) pair; built only with `-DVTEXTENSION_FUZZING=ON` on Clang.
- [ ] **Step 7: Run.** `out/clang-asan/src/vtextension/vtextension_test "PduCodec.*" "Channel.*" "PduStream.*" "WasmBinary.*"` → PASS.
- [ ] **Step 8: Commit.** `git commit -s -m "feat(vtextension): a bounded wire protocol and channels for the extension host process"`

