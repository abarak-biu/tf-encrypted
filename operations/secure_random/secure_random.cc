#include "tensorflow/core/framework/op.h"
#include "tensorflow/core/kernels/bounds_check.h"
#include "tensorflow/core/framework/shape_inference.h"
#include "tensorflow/core/framework/op_kernel.h"
#include "tensorflow/core/lib/random/random_distributions.h"
#include "sodium.h"

using namespace tensorflow;

using shape_inference::DimensionHandle;
using shape_inference::ShapeHandle;

#define CHACHABLOCKSIZE 64

static Status SecureRandomShape(shape_inference::InferenceContext* context) {
  // Check seed shape
  ShapeHandle seed;
  TF_RETURN_IF_ERROR(context->WithRank(context->input(1), 1, &seed));
  DimensionHandle unused;
  TF_RETURN_IF_ERROR(context->WithValue(context->Dim(seed, 0), 8, &unused));

  // Set output shape
  ShapeHandle out;
  TF_RETURN_IF_ERROR(context->MakeShapeFromShapeTensor(0, &out));
  context->set_output(0, out);
  return Status::OK();
}

REGISTER_OP("SecureRandom")
    .Input("shape: T")
    .Input("seed: Tseed")
    .Input("minval: dtype")
    .Input("maxval: dtype")
    .Output("output: dtype")
    .Attr("dtype: {int32, int64} = DT_INT32")
    .Attr("T: {int32, int64} = DT_INT32")
    .Attr("Tseed: {int32} = DT_INT32")
    .SetShapeFn(SecureRandomShape);

// this function allows you to skip ahead in the chacha stream so you don't
// have to allocate more memory than you need to, used in rejection sampling and
// will be handy for parallelizing this operation
void randombytes_buf_deterministic_ic(void * const buf, const size_t size, uint32_t count,
                              const unsigned char seed[randombytes_SEEDBYTES])
{
    static const unsigned char nonce[crypto_stream_chacha20_ietf_NONCEBYTES] = {
        'L', 'i', 'b', 's', 'o', 'd', 'i', 'u', 'm', 'D', 'R', 'G'
    };

    unsigned char * u_buf = (unsigned char *)buf;

    memset(u_buf, 0, size);

    crypto_stream_chacha20_ietf_xor_ic(u_buf, u_buf, (unsigned long long) size,
                                       nonce, count, seed);
}


template <typename T>
class Generator {
public:
  Tensor *output = NULL;
  const unsigned char * seeds = NULL;

  Generator(Tensor* output, const unsigned char * seeds) : output(output), seeds(seeds) {
    auto flat = output->flat<T>();

    count_ = output->flat<T>().size();
    bytes_count_ = count_ * sizeof(T);
    buf_ = static_cast<T *>(malloc(bytes_count_));

    elements_per_block_ = CHACHABLOCKSIZE / sizeof(T);
    extra_block_ = static_cast<T *>(malloc(CHACHABLOCKSIZE));

    block_counter_ = bytes_count_ / CHACHABLOCKSIZE + 1;

    // prepare the extra block if any values get rejected in the rejection sampling
    randombytes_buf_deterministic_ic(extra_block_, CHACHABLOCKSIZE, block_counter_, seeds);
  }

  ~ Generator() {
    free(buf_);
    free(extra_block_);
  }

  void GenerateData(int minval, int maxval) {    
    auto flat = output->flat<T>();

    randombytes_buf_deterministic(buf_, bytes_count_, seeds);

    Uniform(minval, maxval);

    std::copy(buf_, buf_ + flat.size(), flat.data());
  }

private:
  T *buf_ = NULL;
  int count_ = 0;
  int bytes_count_ = 0;

  T * extra_block_ = NULL;
  uint32_t block_counter_ = 0;
  int elements_per_block_ = 0;
  int inner_block_index_ = 0;

  void Uniform(T lo, T hi) {
    auto range = static_cast<typename std::make_unsigned<T>::type>(hi) -
                   static_cast<typename std::make_unsigned<T>::type>(lo);

    typename std::make_unsigned<T>::type min = (1U + ~hi) % hi;

    for (int i = 0; i < count_; ++i) {
      auto unsign = static_cast<typename std::make_unsigned<T>::type>(buf_[i]);
        while(unsign < min) {
          // rejection sampling, get the next valid number in the stream
          buf_[i] = GetNextValidData();
          unsign = static_cast<typename std::make_unsigned<T>::type>(buf_[i]);
        }
        buf_[i] = random::SignedAdd(lo, buf_[i] % range);
    }
  }

  T GetNextValidData() {
    // if the extra block has been used up get the next available block
    if(inner_block_index_ + 1 == elements_per_block_) {
      inner_block_index_ = 0;
      block_counter_++;

      randombytes_buf_deterministic_ic(extra_block_, CHACHABLOCKSIZE, block_counter_, seeds);
    }

    T ret = extra_block_[inner_block_index_];
    inner_block_index_++;

    return ret;
  }
};

template <typename T>
class SecureRandomOp : public OpKernel {
public:
  explicit SecureRandomOp(OpKernelConstruction* context) : OpKernel(context) {}

  void Compute(OpKernelContext* context) override {
    const Tensor& shape_t = context->input(0);
    const Tensor& seed_t = context->input(1);
    const Tensor& minval = context->input(2);
    const Tensor& maxval = context->input(3);
    TensorShape shape;
    OP_REQUIRES_OK(context, MakeShape(shape_t, &shape));
    OP_REQUIRES(context, seed_t.dims() == 1 && seed_t.dim_size(0) == 8,
                errors::InvalidArgument("seed must have shape [8], not ",
                                        seed_t.shape().DebugString()));

    OP_REQUIRES(context, TensorShapeUtils::IsScalar(maxval.shape()),
                errors::InvalidArgument("maxval must be 0-D, got shape ",
                                        maxval.shape().DebugString()));

    T hi = maxval.scalar<T>()();
    T lo = minval.scalar<T>()();
    OP_REQUIRES(
      context, lo < hi,
      errors::InvalidArgument("Need minval < maxval, got ", lo, " >= ", hi));

    // Allocate output
    Tensor* output;
    OP_REQUIRES_OK(context, context->allocate_output(0, shape, &output));
    if (shape.num_elements() == 0) return;

    if (sodium_init() < 0) {
      return;
    }

    int number_of_seeds = randombytes_SEEDBYTES / sizeof(int32);

    int32 *seeds = static_cast<int *>(malloc(sizeof(int32) * number_of_seeds));
    auto seed_vals = seed_t.flat<int32>().data();

    for(auto i = 0; i < number_of_seeds; i++) {
      seeds[i] = seed_vals[i];
    }

    const unsigned char * seed_bytes = reinterpret_cast<const unsigned char*>(seeds);

    Generator<T> gen(output, seed_bytes);

    gen.GenerateData(lo, hi);

    free(seeds);
  }
};


REGISTER_KERNEL_BUILDER(
  Name("SecureRandom")
  .Device(DEVICE_CPU)
  .TypeConstraint<int32>("dtype"),
  SecureRandomOp<int32>);
REGISTER_KERNEL_BUILDER(
  Name("SecureRandom")
  .Device(DEVICE_CPU)
  .TypeConstraint<int64>("dtype"),
  SecureRandomOp<int64>);
