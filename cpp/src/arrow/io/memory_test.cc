// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include <gtest/gtest.h>

#include "arrow/buffer.h"
#include "arrow/io/interfaces.h"
#include "arrow/io/memory.h"
#include "arrow/io/slow.h"
#include "arrow/status.h"
#include "arrow/testing/gtest_util.h"
#include "arrow/testing/util.h"
#include "arrow/util/checked_cast.h"
#include "arrow/util/iterator.h"

namespace arrow {

using internal::checked_cast;

namespace io {

class TestBufferOutputStream : public ::testing::Test {
 public:
  void SetUp() {
    ASSERT_OK(AllocateResizableBuffer(0, &buffer_));
    stream_.reset(new BufferOutputStream(buffer_));
  }

 protected:
  std::shared_ptr<ResizableBuffer> buffer_;
  std::unique_ptr<OutputStream> stream_;
};

TEST_F(TestBufferOutputStream, DtorCloses) {
  std::string data = "data123456";

  const int K = 100;
  for (int i = 0; i < K; ++i) {
    ARROW_EXPECT_OK(stream_->Write(data));
  }

  stream_ = nullptr;
  ASSERT_EQ(static_cast<int64_t>(K * data.size()), buffer_->size());
}

TEST_F(TestBufferOutputStream, CloseResizes) {
  std::string data = "data123456";

  const int K = 100;
  for (int i = 0; i < K; ++i) {
    ARROW_EXPECT_OK(stream_->Write(data));
  }

  ASSERT_OK(stream_->Close());
  ASSERT_EQ(static_cast<int64_t>(K * data.size()), buffer_->size());
}

TEST_F(TestBufferOutputStream, WriteAfterFinish) {
  std::string data = "data123456";
  ASSERT_OK(stream_->Write(data));

  auto buffer_stream = checked_cast<BufferOutputStream*>(stream_.get());

  ASSERT_OK(buffer_stream->Finish());

  ASSERT_RAISES(IOError, stream_->Write(data));
}

TEST_F(TestBufferOutputStream, Reset) {
  std::string data = "data123456";

  auto stream = checked_cast<BufferOutputStream*>(stream_.get());

  ASSERT_OK(stream->Write(data));

  ASSERT_OK_AND_ASSIGN(auto buffer, stream->Finish());
  ASSERT_EQ(buffer->size(), static_cast<int64_t>(data.size()));

  ASSERT_OK(stream->Reset(2048));
  ASSERT_OK(stream->Write(data));
  ASSERT_OK(stream->Write(data));
  ASSERT_OK_AND_ASSIGN(auto buffer2, stream->Finish());

  ASSERT_EQ(buffer2->size(), static_cast<int64_t>(data.size() * 2));
}

TEST(TestFixedSizeBufferWriter, Basics) {
  std::shared_ptr<Buffer> buffer;
  ASSERT_OK(AllocateBuffer(1024, &buffer));

  FixedSizeBufferWriter writer(buffer);

  ASSERT_OK_AND_EQ(0, writer.Tell());

  std::string data = "data123456";
  auto nbytes = static_cast<int64_t>(data.size());
  ASSERT_OK(writer.Write(data.c_str(), nbytes));

  ASSERT_OK_AND_EQ(nbytes, writer.Tell());

  ASSERT_OK(writer.Seek(4));
  ASSERT_OK_AND_EQ(4, writer.Tell());

  ASSERT_OK(writer.Seek(1024));
  ASSERT_OK_AND_EQ(1024, writer.Tell());

  // Write out of bounds
  ASSERT_RAISES(IOError, writer.Write(data.c_str(), 1));

  // Seek out of bounds
  ASSERT_RAISES(IOError, writer.Seek(-1));
  ASSERT_RAISES(IOError, writer.Seek(1025));

  ASSERT_OK(writer.Close());
}

TEST(TestBufferReader, FromStrings) {
  // ARROW-3291: construct BufferReader from std::string or
  // arrow::util::string_view

  std::string data = "data123456";
  auto view = util::string_view(data);

  BufferReader reader1(data);
  BufferReader reader2(view);

  std::shared_ptr<Buffer> piece;
  ASSERT_OK_AND_ASSIGN(piece, reader1.Read(4));
  ASSERT_EQ(0, memcmp(piece->data(), data.data(), 4));

  ASSERT_OK(reader2.Seek(2));
  ASSERT_OK_AND_ASSIGN(piece, reader2.Read(4));
  ASSERT_EQ(0, memcmp(piece->data(), data.data() + 2, 4));
}

TEST(TestBufferReader, Seeking) {
  std::string data = "data123456";

  BufferReader reader(data);
  ASSERT_OK_AND_EQ(0, reader.Tell());

  ASSERT_OK(reader.Seek(9));
  ASSERT_OK_AND_EQ(9, reader.Tell());

  ASSERT_OK(reader.Seek(10));
  ASSERT_OK_AND_EQ(10, reader.Tell());

  ASSERT_RAISES(IOError, reader.Seek(11));
  ASSERT_OK_AND_EQ(10, reader.Tell());
}

TEST(TestBufferReader, Peek) {
  std::string data = "data123456";

  BufferReader reader(std::make_shared<Buffer>(data));

  util::string_view view;

  ASSERT_OK_AND_ASSIGN(view, reader.Peek(4));

  ASSERT_EQ(4, view.size());
  ASSERT_EQ(data.substr(0, 4), view.to_string());

  ASSERT_OK_AND_ASSIGN(view, reader.Peek(20));
  ASSERT_EQ(data.size(), view.size());
  ASSERT_EQ(data, view.to_string());
}

TEST(TestBufferReader, RetainParentReference) {
  // ARROW-387
  std::string data = "data123456";

  std::shared_ptr<Buffer> slice1;
  std::shared_ptr<Buffer> slice2;
  {
    std::shared_ptr<Buffer> buffer;
    ASSERT_OK(AllocateBuffer(nullptr, static_cast<int64_t>(data.size()), &buffer));
    std::memcpy(buffer->mutable_data(), data.c_str(), data.size());
    BufferReader reader(buffer);
    ASSERT_OK_AND_ASSIGN(slice1, reader.Read(4));
    ASSERT_OK_AND_ASSIGN(slice2, reader.Read(6));
  }

  ASSERT_TRUE(slice1->parent() != nullptr);

  ASSERT_EQ(0, std::memcmp(slice1->data(), data.c_str(), 4));
  ASSERT_EQ(0, std::memcmp(slice2->data(), data.c_str() + 4, 6));
}

TEST(TestRandomAccessFile, GetStream) {
  std::string data = "data1data2data3data4data5";

  auto buf = std::make_shared<Buffer>(data);
  auto file = std::make_shared<BufferReader>(buf);

  std::shared_ptr<InputStream> stream1, stream2;

  stream1 = RandomAccessFile::GetStream(file, 0, 10);
  stream2 = RandomAccessFile::GetStream(file, 9, 16);

  ASSERT_OK_AND_EQ(0, stream1->Tell());

  std::shared_ptr<Buffer> buf2;
  uint8_t buf3[20];

  ASSERT_OK_AND_EQ(4, stream2->Read(4, buf3));
  ASSERT_EQ(0, std::memcmp(buf3, "2dat", 4));
  ASSERT_OK_AND_EQ(4, stream2->Tell());

  ASSERT_OK_AND_EQ(6, stream1->Read(6, buf3));
  ASSERT_EQ(0, std::memcmp(buf3, "data1d", 6));
  ASSERT_OK_AND_EQ(6, stream1->Tell());

  ASSERT_OK_AND_ASSIGN(buf2, stream1->Read(2));
  ASSERT_TRUE(SliceBuffer(buf, 6, 2)->Equals(*buf2));

  // Read to end of each stream
  ASSERT_OK_AND_EQ(2, stream1->Read(4, buf3));
  ASSERT_EQ(0, std::memcmp(buf3, "a2", 2));
  ASSERT_OK_AND_EQ(10, stream1->Tell());

  ASSERT_OK_AND_EQ(0, stream1->Read(1, buf3));
  ASSERT_OK_AND_EQ(10, stream1->Tell());

  // stream2 had its extent limited
  ASSERT_OK_AND_ASSIGN(buf2, stream2->Read(20));
  ASSERT_TRUE(SliceBuffer(buf, 13, 12)->Equals(*buf2));

  ASSERT_OK_AND_ASSIGN(buf2, stream2->Read(1));
  ASSERT_EQ(0, buf2->size());
  ASSERT_OK_AND_EQ(16, stream2->Tell());

  ASSERT_OK(stream1->Close());

  // idempotent
  ASSERT_OK(stream1->Close());
  ASSERT_TRUE(stream1->closed());

  // Check whether closed
  ASSERT_RAISES(IOError, stream1->Tell());
  ASSERT_RAISES(IOError, stream1->Read(1));
  ASSERT_RAISES(IOError, stream1->Read(1, buf3));
}

TEST(TestMemcopy, ParallelMemcopy) {
#if defined(ARROW_VALGRIND)
  // Compensate for Valgrind's slowness
  constexpr int64_t THRESHOLD = 32 * 1024;
#else
  constexpr int64_t THRESHOLD = 1024 * 1024;
#endif

  for (int i = 0; i < 5; ++i) {
    // randomize size so the memcopy alignment is tested
    int64_t total_size = 3 * THRESHOLD + std::rand() % 100;

    std::shared_ptr<Buffer> buffer1, buffer2;

    ASSERT_OK(AllocateBuffer(total_size, &buffer1));
    ASSERT_OK(AllocateBuffer(total_size, &buffer2));

    random_bytes(total_size, 0, buffer2->mutable_data());

    io::FixedSizeBufferWriter writer(buffer1);
    writer.set_memcopy_threads(4);
    writer.set_memcopy_threshold(THRESHOLD);
    ASSERT_OK(writer.Write(buffer2->data(), buffer2->size()));

    ASSERT_EQ(0, memcmp(buffer1->data(), buffer2->data(), buffer1->size()));
  }
}

template <typename SlowStreamType>
void TestSlowInputStream() {
  using clock = std::chrono::high_resolution_clock;

  auto stream = std::make_shared<BufferReader>(util::string_view("abcdefghijkl"));
  const double latency = 0.6;
  auto slow = std::make_shared<SlowStreamType>(stream, latency);

  ASSERT_FALSE(slow->closed());
  auto t1 = clock::now();
  ASSERT_OK_AND_ASSIGN(auto buf, slow->Read(6));
  auto t2 = clock::now();
  AssertBufferEqual(*buf, "abcdef");
  auto dt = std::chrono::duration_cast<std::chrono::duration<double>>(t2 - t1).count();
#ifdef ARROW_WITH_TIMING_TESTS
  ASSERT_LT(dt, latency * 3);  // likely
  ASSERT_GT(dt, latency / 3);  // likely
#else
  ARROW_UNUSED(dt);
#endif

  ASSERT_OK_AND_ASSIGN(util::string_view view, slow->Peek(4));
  ASSERT_EQ(view, util::string_view("ghij"));

  ASSERT_OK(slow->Close());
  ASSERT_TRUE(slow->closed());
  ASSERT_TRUE(stream->closed());
  ASSERT_OK(slow->Close());
  ASSERT_TRUE(slow->closed());
  ASSERT_TRUE(stream->closed());
}

TEST(TestSlowInputStream, Basics) { TestSlowInputStream<SlowInputStream>(); }

TEST(TestSlowRandomAccessFile, Basics) { TestSlowInputStream<SlowRandomAccessFile>(); }

TEST(TestInputStreamIterator, Basics) {
  auto reader = std::make_shared<BufferReader>(Buffer::FromString("data123456"));
  ASSERT_OK_AND_ASSIGN(auto it, MakeInputStreamIterator(reader, /*block_size=*/3));
  std::shared_ptr<Buffer> buf;
  ASSERT_OK_AND_ASSIGN(buf, it.Next());
  AssertBufferEqual(*buf, "dat");
  ASSERT_OK_AND_ASSIGN(buf, it.Next());
  AssertBufferEqual(*buf, "a12");
  ASSERT_OK_AND_ASSIGN(buf, it.Next());
  AssertBufferEqual(*buf, "345");
  ASSERT_OK_AND_ASSIGN(buf, it.Next());
  AssertBufferEqual(*buf, "6");
  ASSERT_OK_AND_ASSIGN(buf, it.Next());
  ASSERT_EQ(buf, nullptr);
  ASSERT_OK_AND_ASSIGN(buf, it.Next());
  ASSERT_EQ(buf, nullptr);
}

TEST(TestInputStreamIterator, Closed) {
  auto reader = std::make_shared<BufferReader>(Buffer::FromString("data123456"));
  ASSERT_OK(reader->Close());
  ASSERT_RAISES(Invalid, MakeInputStreamIterator(reader, 3));

  reader = std::make_shared<BufferReader>(Buffer::FromString("data123456"));
  ASSERT_OK_AND_ASSIGN(auto it, MakeInputStreamIterator(reader, /*block_size=*/3));
  ASSERT_OK_AND_ASSIGN(auto buf, it.Next());
  AssertBufferEqual(*buf, "dat");
  // Close stream and read from iterator
  ASSERT_OK(reader->Close());
  ASSERT_RAISES(Invalid, it.Next().status());
}

}  // namespace io
}  // namespace arrow
