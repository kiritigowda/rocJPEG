/*
Copyright (c) 2024 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "../rocjpeg_samples_utils.h"

void ThreadFunction(std::vector<std::string>& jpegFiles, RocJpegHandle rocjpeg_handle, RocJpegStreamHandle rocjpeg_stream, RocJpegUtils rocjpeg_util, RocJpegImage *output_image, std::mutex &mutex,
    RocJpegDecodeParams &decode_params, bool save_images, std::string &output_file_path, uint64_t *num_decoded_images, double *image_size_in_mpixels, uint64_t *num_bad_jpegs, uint64_t *num_jpegs_with_411_subsampling,
    uint64_t *num_jpegs_with_unknown_subsampling, uint64_t *num_jpegs_with_unsupported_resolution) {

    bool is_roi_valid = false;
    uint32_t roi_width;
    uint32_t roi_height;
    roi_width = decode_params.crop_rectangle.right - decode_params.crop_rectangle.left;
    roi_height = decode_params.crop_rectangle.bottom - decode_params.crop_rectangle.top;

    std::vector<char> file_data;
    uint8_t num_components;
    uint32_t widths[ROCJPEG_MAX_COMPONENT] = {};
    uint32_t heights[ROCJPEG_MAX_COMPONENT] = {};
    uint32_t channel_sizes[ROCJPEG_MAX_COMPONENT] = {};
    uint32_t prior_channel_sizes[ROCJPEG_MAX_COMPONENT] = {};
    RocJpegChromaSubsampling subsampling;
    std::string chroma_sub_sampling = "";
    uint32_t num_channels = 0;

    while (true) {
        // Get the next JPEG file to process
        std::string file_path;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (!jpegFiles.empty()) {
                file_path = jpegFiles.front();
                jpegFiles.erase(jpegFiles.begin());
            }
        }
        if (file_path.empty()) {
            // No more files to process
            break;
        }

        std::string base_file_name = file_path.substr(file_path.find_last_of("/\\") + 1);
        // Read an image from disk.
        std::ifstream input(file_path.c_str(), std::ios::in | std::ios::binary | std::ios::ate);
        if (!(input.is_open())) {
            std::cerr << "ERROR: Cannot open image: " << file_path << std::endl;
            return;
        }
        // Get the size
        std::streamsize file_size = input.tellg();
        input.seekg(0, std::ios::beg);
        // resize if buffer is too small
        if (file_data.size() < file_size) {
            file_data.resize(file_size);
        }
        if (!input.read(file_data.data(), file_size)) {
            std::cerr << "ERROR: Cannot read from file: " << file_path << std::endl;
            return;
        }

        RocJpegStatus rocjpeg_status = rocJpegStreamParse(reinterpret_cast<uint8_t *>(file_data.data()), file_size, rocjpeg_stream);
        if (rocjpeg_status != ROCJPEG_STATUS_SUCCESS) {
            std::cerr << "Skipping decoding input file: " << file_path << std::endl;
            *num_bad_jpegs += 1;
            continue;
        }

        CHECK_ROCJPEG(rocJpegGetImageInfo(rocjpeg_handle, rocjpeg_stream, &num_components, &subsampling, widths, heights));
        if (roi_width > 0 && roi_height > 0 && roi_width <= widths[0] && roi_height <= heights[0]) {
            is_roi_valid = true; 
        }

        if (widths[0] < 64 || heights[0] < 64) {
            *num_jpegs_with_unsupported_resolution += 1;
            continue;
        }

        if (subsampling == ROCJPEG_CSS_411 || subsampling == ROCJPEG_CSS_UNKNOWN) {
            if (subsampling == ROCJPEG_CSS_411) {
                *num_jpegs_with_411_subsampling += 1;
            }
            if (subsampling == ROCJPEG_CSS_UNKNOWN) {
                *num_jpegs_with_unknown_subsampling += 1;
            }

            continue;
        }

        if (rocjpeg_util.GetChannelPitchAndSizes(decode_params, subsampling, widths, heights, num_channels, *output_image, channel_sizes)) {
            std::cerr << "ERROR: Failed to get the channel pitch and sizes" << std::endl;
            return;
        }

        // allocate memory for each channel
        for (int i = 0; i < num_channels; i++) {
            if (prior_channel_sizes[i] != channel_sizes[i]) {
                if (output_image->channel[i] != nullptr) {
                    CHECK_HIP(hipFree((void*)output_image->channel[i]));
                    output_image->channel[i] = nullptr;
                }
                CHECK_HIP(hipMalloc(&output_image->channel[i], channel_sizes[i]));
            }
        }

        CHECK_ROCJPEG(rocJpegDecode(rocjpeg_handle, rocjpeg_stream, &decode_params, output_image));
        *image_size_in_mpixels += (static_cast<double>(widths[0]) * static_cast<double>(heights[0]) / 1000000);
        *num_decoded_images += 1;

        if (save_images) {
            std::string image_save_path = output_file_path;
            //if ROI is present, need to pass roi_width and roi_height
            uint32_t width = is_roi_valid ? roi_width : widths[0];
            uint32_t height = is_roi_valid ? roi_height : heights[0];
            rocjpeg_util.GetOutputFileExt(decode_params.output_format, base_file_name, width, height, subsampling, image_save_path);
            rocjpeg_util.SaveImage(image_save_path, output_image, width, height, subsampling, decode_params.output_format);
        }

        for (int i = 0; i < ROCJPEG_MAX_COMPONENT; i++) {
            prior_channel_sizes[i] = channel_sizes[i];
        }

    }
}

int main(int argc, char **argv) {
    int device_id = 0;
    bool save_images = false;
    int num_threads = 2;
    int total_images_all = 0;
    double image_per_sec_all = 0;
    std::string input_path, output_file_path;
    std::vector<std::string> file_paths = {};
    bool is_dir = false;
    bool is_file = false;
    RocJpegChromaSubsampling subsampling;
    RocJpegBackend rocjpeg_backend = ROCJPEG_BACKEND_HARDWARE;
    RocJpegDecodeParams decode_params = {};
    std::vector<RocJpegHandle> rocjpeg_handles;
    std::vector<RocJpegStreamHandle> rocjpeg_streams;
    std::mutex mutex;
    std::vector<uint64_t> num_decoded_images_per_thread;
    std::vector<double> image_size_in_mpixels_per_thread;
    std::vector<RocJpegImage> rocjpeg_images;
    RocJpegUtils rocjpeg_utils;
    std::vector<std::thread> threads;
    std::vector<uint64_t> num_bad_jpegs;
    std::vector<uint64_t> num_jpegs_with_411_subsampling;
    std::vector<uint64_t> num_jpegs_with_unknown_subsampling;
    std::vector<uint64_t> num_jpegs_with_unsupported_resolution;

    RocJpegUtils::ParseCommandLine(input_path, output_file_path, save_images, device_id, rocjpeg_backend, decode_params, &num_threads, nullptr, argc, argv);
    if (!RocJpegUtils::GetFilePaths(input_path, file_paths, is_dir, is_file)) {
        std::cerr << "ERROR: Failed to get input file paths!" << std::endl;
        return EXIT_FAILURE;
    }
    if (!RocJpegUtils::InitHipDevice(device_id)) {
        std::cerr << "ERROR: Failed to initialize HIP!" << std::endl;
        return EXIT_FAILURE;
    }

    if (num_threads > file_paths.size()) {
        num_threads = file_paths.size();
    }

    std::cout << "Creating decoder objects, please wait!" << std::endl;
    for (int i = 0; i < num_threads; i++) {
        RocJpegStreamHandle rocjpeg_stream;
        RocJpegHandle rocjpeg_handle;
        CHECK_ROCJPEG(rocJpegCreate(rocjpeg_backend, device_id, &rocjpeg_handle));
        rocjpeg_handles.push_back(std::move(rocjpeg_handle));
        CHECK_ROCJPEG(rocJpegStreamCreate(&rocjpeg_stream));
        rocjpeg_streams.push_back(std::move(rocjpeg_stream));
    }
    num_decoded_images_per_thread.resize(num_threads, 0);
    image_size_in_mpixels_per_thread.resize(num_threads, 0);
    rocjpeg_images.resize(num_threads, {0});
    num_bad_jpegs.resize(num_threads, 0);
    num_jpegs_with_411_subsampling.resize(num_threads, 0);
    num_jpegs_with_unknown_subsampling.resize(num_threads, 0);
    num_jpegs_with_unsupported_resolution.resize(num_threads, 0);

    std::cout << "Decoding started with " << num_threads << " threads, please wait!" << std::endl;
    auto start_time = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < num_threads; ++i) {
        threads.emplace_back(ThreadFunction, std::ref(file_paths), rocjpeg_handles[i], rocjpeg_streams[i], rocjpeg_utils, &rocjpeg_images[i], std::ref(mutex), std::ref(decode_params), save_images, std::ref(output_file_path),
            &num_decoded_images_per_thread[i], &image_size_in_mpixels_per_thread[i], &num_bad_jpegs[i], &num_jpegs_with_411_subsampling[i], &num_jpegs_with_unknown_subsampling[i], &num_jpegs_with_unsupported_resolution[i]);
    }
    for (auto& thread : threads) {
        thread.join();
    }
    auto end_time = std::chrono::high_resolution_clock::now();
    auto total_time_in_milli_sec = std::chrono::duration<double, std::milli>(end_time - start_time).count();

    uint64_t total_decoded_images = 0;
    double total_image_size_in_mpixels = 0;
    uint64_t total_num_bad_jpegs = 0;
    uint64_t total_num_jpegs_with_411_subsampling = 0;
    uint64_t total_num_jpegs_with_unknown_subsampling = 0;
    uint64_t total_num_jpegs_with_unsupported_resolution = 0;

    for (auto i = 0 ; i < num_threads; i++) {
        total_decoded_images += num_decoded_images_per_thread[i];
        total_image_size_in_mpixels += image_size_in_mpixels_per_thread[i];
        total_num_bad_jpegs += num_bad_jpegs[i];
        total_num_jpegs_with_411_subsampling += num_jpegs_with_411_subsampling[i];
        total_num_jpegs_with_unknown_subsampling += num_jpegs_with_unknown_subsampling[i];
        total_num_jpegs_with_unsupported_resolution += num_jpegs_with_unsupported_resolution[i];
        for (int j = 0; j < ROCJPEG_MAX_COMPONENT; j++) {
            if (rocjpeg_images[i].channel[j] != nullptr) {
                CHECK_HIP(hipFree((void *)rocjpeg_images[i].channel[j]));
                rocjpeg_images[i].channel[j] = nullptr;
            }
        }
    }

    double average_decoding_time_in_milli_sec = total_time_in_milli_sec / total_decoded_images;
    double avg_images_per_sec = 1000 / average_decoding_time_in_milli_sec;
    double avg_image_size_in_mpixels_per_sec = total_image_size_in_mpixels * avg_images_per_sec / total_decoded_images;
    std::cout << "Total elapsed time (ms): " << total_time_in_milli_sec << std::endl;
    std::cout << "Total decoded images: " << total_decoded_images << std::endl;
    if (total_num_bad_jpegs || total_num_jpegs_with_411_subsampling || total_num_jpegs_with_unknown_subsampling || total_num_jpegs_with_unsupported_resolution) {
        std::cout << "Total skipped images: " << total_num_bad_jpegs + total_num_jpegs_with_411_subsampling + total_num_jpegs_with_unknown_subsampling + total_num_jpegs_with_unsupported_resolution;
        if (total_num_bad_jpegs) {
            std::cout << " ,total images that cannot be parsed: " << total_num_bad_jpegs;
        }
        if (total_num_jpegs_with_411_subsampling) {
            std::cout << " ,total images with YUV 4:1:1 chroam subsampling: " << total_num_jpegs_with_411_subsampling;
        }
        if (total_num_jpegs_with_unknown_subsampling) {
            std::cout << " ,total images with unknwon chroam subsampling: " << total_num_jpegs_with_unknown_subsampling;
        }
        if (total_num_jpegs_with_unsupported_resolution) {
            std::cout << " ,total images with unsupported_resolution: " << total_num_jpegs_with_unsupported_resolution;
        }
        std::cout << std::endl;
    }

    if (total_decoded_images > 0) {
        std::cout << "Average processing time per image (ms): " << average_decoding_time_in_milli_sec << std::endl;
        std::cout << "Average decoded images per sec (Images/Sec): " << avg_images_per_sec << std::endl;
        std::cout << "Average decoded images size (Mpixels/Sec): " << avg_image_size_in_mpixels_per_sec << std::endl;
    }

    for (auto& handle : rocjpeg_handles) {
        CHECK_ROCJPEG(rocJpegDestroy(handle));
    }
    for (auto& rocjpecg_stream : rocjpeg_streams) {
        CHECK_ROCJPEG(rocJpegStreamDestroy(rocjpecg_stream));
    }
    std::cout << "Decoding completed!" << std::endl;
    return EXIT_SUCCESS;
}