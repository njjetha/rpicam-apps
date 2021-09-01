/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (C) 2021, Raspberry Pi (Trading) Limited
 *
 * tf_stage.hpp - base class for TensorFlowLite stages
 */

#include "tf_stage.hpp"

TfStage::TfStage(LibcameraApp *app, int tf_w, int tf_h) : PostProcessingStage(app), tf_w_(tf_w), tf_h_(tf_h)
{
	if (tf_w_ <= 0 || tf_h_ <= 0)
		throw std::runtime_error("TfStage: Bad TFLite input dimensions");
}

void TfStage::Read(boost::property_tree::ptree const &params)
{
	config_->number_of_threads = params.get<int>("number_of_threads", 2);
	config_->refresh_rate = params.get<int>("refresh_rate", 5);
	config_->model_file = params.get<std::string>("model_file", "");
	config_->verbose = params.get<int>("verbose", 0);
	config_->normalisation_offset = params.get<float>("normalisation_offset", 127.5);
	config_->normalisation_scale = params.get<float>("normalisation_scale", 127.5);

	initialise();

	readExtras(params);
}

void TfStage::initialise()
{
	model_ = tflite::FlatBufferModel::BuildFromFile(config_->model_file.c_str());
	if (!model_)
		throw std::runtime_error("TfStage: Failed to load model");
	std::cout << "TfStage: Loaded model " << config_->model_file << std::endl;

	tflite::ops::builtin::BuiltinOpResolver resolver;
	tflite::InterpreterBuilder(*model_, resolver)(&interpreter_);
	if (!interpreter_)
		throw std::runtime_error("TfStage: Failed to construct interpreter");

	if (config_->number_of_threads != -1)
		interpreter_->SetNumThreads(config_->number_of_threads);

	if (interpreter_->AllocateTensors() != kTfLiteOk)
		throw std::runtime_error("TfStage: Failed to allocate tensors");

	// Make an attempt to verify that the model expects this size of input.
	int input = interpreter_->inputs()[0];
	size_t size = interpreter_->tensor(input)->bytes;
	size_t check = tf_w_ * tf_h_ * 3; // assume RGB
	if (interpreter_->tensor(input)->type == kTfLiteUInt8)
		check *= sizeof(uint8_t);
	else if (interpreter_->tensor(input)->type == kTfLiteFloat32)
		check *= sizeof(float);
	else
		throw std::runtime_error("TfStage: Input tensor data type not supported");

	// Causes might include loading the wrong model.
	if (check != size)
		throw std::runtime_error("TfStage: Input tensor size mismatch");
}

void TfStage::Configure()
{
	lores_w_ = lores_h_ = lores_stride_ = 0;
	lores_stream_ = app_->LoresStream();
	if (lores_stream_)
	{
		app_->StreamDimensions(lores_stream_, &lores_w_, &lores_h_, &lores_stride_);
		if (config_->verbose)
			std::cout << "TfStage: Low resolution stream is " << lores_w_ << "x" << lores_h_ << std::endl;
		if (tf_w_ > lores_w_ || tf_h_ > lores_h_)
		{
			std::cout << "TfStage: WARNING: Low resolution image too small" << std::endl;
			lores_stream_ = nullptr;
		}
	}
	else if (config_->verbose)
		std::cout << "TfStage: no low resolution stream" << std::endl;

	main_w_ = main_h_ = main_stride_ = 0;
	main_stream_ = app_->GetMainStream();
	if (main_stream_)
	{
		app_->StreamDimensions(main_stream_, &main_w_, &main_h_, &main_stride_);
		if (config_->verbose)
			std::cout << "TfStage: Main stream is " << main_w_ << "x" << main_h_ << std::endl;
	}
	else if (config_->verbose)
		std::cout << "TfStage: No main stream" << std::endl;

	checkConfiguration();
}

bool TfStage::Process(CompletedRequest &completed_request)
{
	if (!lores_stream_)
		return false;

	{
		std::unique_lock<std::mutex> lck(future_mutex_);
		if (config_->refresh_rate && completed_request.sequence % config_->refresh_rate == 0 &&
			(!future_ || future_->wait_for(std::chrono::seconds(0)) == std::future_status::ready))
		{
			libcamera::Span<uint8_t> buffer = app_->Mmap(completed_request.buffers[lores_stream_])[0];

			tensor_input_ = yuvToRgb(buffer.data());

			future_ = std::make_unique<std::future<void>>();
			*future_ = std::move(std::async(std::launch::async, [this] {
				auto time_taken = ExecutionTime<std::micro>(&TfStage::runInference, this).count();

				if (config_->verbose)
					std::cout << "TfStage: Inference time: " << time_taken << " ms" << std::endl;
			}));
		}
	}

	std::unique_lock<std::mutex> lock(output_mutex_);
	applyResults(completed_request);

	return false;
}

std::vector<uint8_t> TfStage::yuvToRgb(uint8_t *src)
{
	std::vector<uint8_t> output(tf_h_ * tf_w_ * 3);
	uint8_t *dst = &output[0];

	int off_x = ((lores_w_ - tf_w_) / 2) & ~1, off_y = ((lores_h_ - tf_h_) / 2) & ~1;
	int src_size = lores_h_ * lores_stride_, src_U_size = (lores_h_ / 2) * (lores_stride_ / 2);

	for (int y = 0; y < tf_h_; y++)
	{
		uint8_t *src_Y = src + (y + off_y) * lores_stride_ + off_x;
		uint8_t *src_U = src + src_size + ((y + off_y) / 2) * (lores_stride_ / 2) + off_x / 2;
		uint8_t *src_V = src_U + src_U_size;
		for (int x = 0; x < tf_w_; x++)
		{
			int Y = *(src_Y++);
			int U = *(src_U);
			int V = *(src_V);
			src_U += (x & 1);
			src_V += (x & 1);

			int R = std::clamp<int>((Y + (1.402 * (V - 128))), 0, 255);
			int G = std::clamp<int>((Y - 0.345 * (U - 128) - 0.714 * (V - 128)), 0, 255);
			int B = std::clamp<int>((Y + 1.771 * (U - 128)), 0, 255);

			*(dst++) = R;
			*(dst++) = G;
			*(dst++) = B;
		}
	}

	return output;
}

void TfStage::runInference()
{
	int input = interpreter_->inputs()[0];

	if (interpreter_->tensor(input)->type == kTfLiteUInt8)
	{
		uint8_t *tensor = interpreter_->typed_tensor<uint8_t>(input);
		for (int i = 0; i < tensor_input_.size(); i++)
			tensor[i] = tensor_input_[i];
	}
	else if (interpreter_->tensor(input)->type == kTfLiteFloat32)
	{
		float *tensor = interpreter_->typed_tensor<float>(input);
		for (int i = 0; i < tensor_input_.size(); i++)
			tensor[i] = (tensor_input_[i] - config_->normalisation_offset) / config_->normalisation_scale;
	}

	if (interpreter_->Invoke() != kTfLiteOk)
		throw std::runtime_error("TfStage: Failed to invoke TFLite");

	std::unique_lock<std::mutex> lock(output_mutex_);
	interpretOutputs();
}

void TfStage::Stop()
{
	if (future_)
		future_->wait();
}
