#include <vector>
#include <memory>
#include <random>
#include <numeric>
#include "libpressio_ext/cpp/data.h"
#include "libpressio_ext/cpp/compressor.h"
#include "libpressio_ext/cpp/options.h"
#include "libpressio_ext/cpp/pressio.h"
#include "pressio_options.h"
#include "pressio_data.h"
#include "pressio_compressor.h"


class sample_compressor_plugin: public libpressio_compressor_plugin {
  public:
    struct pressio_options get_options_impl() const override {
      struct pressio_options options;
      options.set("sample:mode", mode);
      options.set("sample:seed", seed);
      options.set("sample:rate", rate);
      return options;
    }

    struct pressio_options get_configuration_impl() const override {
      struct pressio_options options;
      options.set("pressio:thread_safe", static_cast<int>(pressio_thread_safety_multiple));
      options.set("sampling:modes", {"wr", "wor", "decimate"});
      return options;
    }

    int set_options_impl(struct pressio_options const& options) override {
      options.get("sample:mode", &mode);
      options.get("sample:seed", &seed);
      options.get("sample:rate", &rate);
      return 0;
    }

    int compress_impl(const pressio_data *input, struct pressio_data* output) override {
      std::vector<size_t> const& dims = input->dimensions();
      size_t sample_size;
      size_t take_rows = 0;
      const size_t total_rows = dims.back();
      if(mode == "wr" || mode == "wor") {
        sample_size = std::floor(rate * total_rows);
      } else if( mode =="decimate") {
        do {
          sample_size = std::ceil(static_cast<double>(total_rows)/++take_rows);
        } while(sample_size/static_cast<double>(total_rows) > rate);
      } else {
        return invalid_mode(mode);
      }

      std::vector<size_t> rows_to_sample;
      std::seed_seq seed_s{seed};
      std::minstd_rand dist{seed_s};

      if(mode == "wr") {
        rows_to_sample.resize(sample_size);
        std::uniform_int_distribution<size_t> gen(0, total_rows-1);
        auto rand = [&]{return gen(dist); };
        std::generate(std::begin(rows_to_sample), std::end(rows_to_sample), rand);
        std::sort(std::begin(rows_to_sample), std::end(rows_to_sample));
      } else if (mode == "wor") {
        rows_to_sample.resize(total_rows);
        std::iota(std::begin(rows_to_sample), std::end(rows_to_sample), 0);
        std::shuffle(std::begin(rows_to_sample), std::end(rows_to_sample), dist);
        rows_to_sample.resize(sample_size);
        std::sort(std::begin(rows_to_sample), std::end(rows_to_sample));
      } else if (mode == "decimate") {
        size_t i = 0;
        std::generate(std::begin(rows_to_sample), std::end(rows_to_sample), [=]() mutable { size_t ret = i; i += take_rows; return ret; });
      } else {
        return 1;
      }

      //actually sample the "rows"
      std::vector<size_t> new_dims = dims;
      new_dims.back() = sample_size;
      *output = pressio_data::owning(input->dtype(), new_dims);
      unsigned char* output_ptr = static_cast<unsigned char*>(output->data());
      unsigned char* input_ptr = static_cast<unsigned char*>(input->data());
      size_t row_size = std::accumulate(
          std::next(std::rbegin(dims)),
          std::rend(dims),
          1ul, std::multiplies<>{}
          ) * pressio_dtype_size(input->dtype());

      for (auto row : rows_to_sample) {
        memcpy(output_ptr, input_ptr + (row*row_size), row_size);
        output_ptr += row_size;
      }

      return 0;
    }

    int decompress_impl(const pressio_data *input, struct pressio_data* output) override {
      *output = pressio_data::clone(*input);
      return 0;
    }

    int major_version() const override {
      return 0;
    }
    int minor_version() const override {
      return 0;
    }
    int patch_version() const override {
      return 1;
    }

    const char* version() const override {
      return "0.0.1";
    }

    const char* prefix() const override {
      return "sample";
    }

    std::shared_ptr<libpressio_compressor_plugin> clone() override {
      return compat::make_unique<sample_compressor_plugin>(*this);
    }


  private:
    std::string mode;
    int seed = 0;
    double rate;
    int invalid_mode(std::string const& mode) {
      return set_error(1, mode + " invalid mode");
    }
};

static pressio_register X(compressor_plugins(), "sample", [](){ return compat::make_unique<sample_compressor_plugin>(); });

