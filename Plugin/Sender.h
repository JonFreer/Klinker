#pragma once

#include "Common.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <tuple>

namespace klinker
{
    //
    // Frame sender class
    //
    // There are two modes that determine how output frames are scheduled.
    //
    // * Async mode
    //
    // Output frames are asynchronously scheduled by the completion callback.
    // Unity can update the frame at any time, but it's not guaranteed to be
    // scheduled, as the completion callback only takes the latest state.
    //
    // The length of the output queue is adjusted by prerolling.
    //
    // * Manual mode
    //
    // Output frames are directly scheduled by Unity. It can guarantee that
    // all frames are to be scheduled on time but needs to be explicitly
    // synchronized to output refreshing. The WaitFrameCompletion method is
    // provided for this purpose.
    //
    // The length of the output queue is controlled by Unity.
    //
    class Sender final : private IDeckLinkVideoOutputCallback
    {
    public:

        #pragma region Constructor/destructor

        ~Sender()
        {
            // Internal objects should have been released.
            assert(output_ == nullptr);
            assert(displayMode_ == nullptr);
            assert(frame_ == nullptr);
        }

        #pragma endregion

        #pragma region Accessor methods

        std::tuple<int, int> GetFrameDimensions() const
        {
            assert(displayMode_ != nullptr);
            return { displayMode_->GetWidth(), displayMode_->GetHeight() };
        }

        std::int64_t GetFrameDuration() const
        {
            return flicksPerSecond * frameDuration_ / timeScale_;
        }

        bool IsProgressive() const
        {
            assert(displayMode_ != nullptr);
            return displayMode_->GetFieldDominance() == bmdProgressiveFrame;
        }

        bool IsReferenceLocked() const
        {
            assert(output_ != nullptr);
            BMDReferenceStatus stat;
            ShouldOK(output_->GetReferenceStatus(&stat));
            return stat & bmdReferenceLocked;
        }

        int CountDroppedFrames() const
        {
            return dropCount_;
        }

        const std::string& GetErrorString() const
        {
            return error_;
        }

        #pragma endregion

        #pragma region Public methods

        void StartAsyncMode(int deviceIndex, int formatIndex, int preroll)
        {
            assert(output_ == nullptr);
            assert(displayMode_ == nullptr);
            assert(frame_ == nullptr);

            if (!InitializeOutput(deviceIndex, formatIndex)) return;

            // Prerolling
            frame_ = AllocateFrame();
            for (auto i = 0; i < preroll; i++) ScheduleFrame(frame_);

            ShouldOK(output_->StartScheduledPlayback(0, 1, 1));
        }

        void StartManualMode(int deviceIndex, int formatIndex)
        {
            assert(output_ == nullptr);
            assert(displayMode_ == nullptr);
            assert(frame_ == nullptr);

            if (!InitializeOutput(deviceIndex, formatIndex)) return;

            ShouldOK(output_->StartScheduledPlayback(0, 1, 1));
        }

        void Stop()
        {
            // Stop the output stream.
            if (output_ != nullptr)
            {
                output_->StopScheduledPlayback(0, nullptr, 1);
                output_->SetScheduledFrameCompletionCallback(nullptr);
                output_->DisableVideoOutput();
            }

            // Release the internal objects.
            if (frame_ != nullptr)
            {
                frame_->Release();
                frame_ = nullptr;
            }

            if (displayMode_ != nullptr)
            {
                displayMode_->Release();
                displayMode_ = nullptr;
            }

            if (output_ != nullptr)
            {
                output_->Release();
                output_ = nullptr;
            }
        }

        void FeedFrame(void* frameData, unsigned int timecode)
        {
            assert(output_ != nullptr);
            assert(displayMode_ != nullptr);
            assert(error_.empty());

            // Allocate a new frame for the fed data.
            auto newFrame = AllocateFrame();
            CopyFrameData(newFrame, frameData);
            SetTimecode(newFrame, timecode);

            if (IsAsyncMode())
            {
                // Async mode: Replace the frame_ object with it.
                std::lock_guard<std::mutex> lock(mutex_);
                frame_->Release();
                frame_ = newFrame;
            }
            else
            {
                // Manual mode: Immediately schedule it.
                ScheduleFrame(newFrame);
                newFrame->Release();

                // Note: We don't have to use the mutex because nothing here
                // conflicts with the completion callback.

            #if defined(_DEBUG)
                // We shouldn't push too many frames to the scheduler.
                unsigned int count;
                ShouldOK(output_->GetBufferedVideoFrameCount(&count));
                assert(count < 20);
            #endif
            }
        }

        void WaitFrameCompletion(std::int64_t frameNumber)
        {
            // Wait for completion of a specified frame.
            std::unique_lock<std::mutex> lock(mutex_);
            auto res = condition_.wait_for(
                lock, std::chrono::milliseconds(200),
                [=]() { return counters_.completed >= frameNumber; }
            );

            if (!res) error_ = "Failed to synchronize to output refreshing.";
        }

        #pragma endregion

        #pragma region IUnknown implementation

        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) override
        {
            if (iid == IID_IUnknown)
            {
                *ppv = this;
                AddRef();
                return S_OK;
            }

            if (iid == IID_IDeckLinkVideoOutputCallback)
            {
                *ppv = (IDeckLinkVideoOutputCallback*)this;
                AddRef();
                return S_OK;
            }

            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        ULONG STDMETHODCALLTYPE AddRef() override
        {
            return refCount_.fetch_add(1);
        }

        ULONG STDMETHODCALLTYPE Release() override
        {
            auto val = refCount_.fetch_sub(1);
            if (val == 1) delete this;
            return val;
        }
        
        #pragma endregion

        #pragma region IDeckLinkVideoOutputCallback implementation

        HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(
            IDeckLinkVideoFrame *completedFrame,
            BMDOutputFrameCompletionResult result
        ) override
        {
            if (result == bmdOutputFrameDisplayedLate)
            {
                DebugLog("Frame was displayed late.");
                dropCount_++;
            }

            if (result == bmdOutputFrameDropped)
            {
                DebugLog("Frame was dropped.");
            }

            std::lock_guard<std::mutex> lock(mutex_);

            // Increment the frame count and notify the main thread.
            counters_.completed++;
            condition_.notify_all();

            // Async mode: Schedule the next frame.
            if (IsAsyncMode()) ScheduleFrame(frame_);

            return S_OK;
        }

        HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override
        {
            return S_OK;
        }

        #pragma endregion

    private:

        #pragma region Private members

        std::atomic<ULONG> refCount_ = 1;
        std::string error_;

        IDeckLinkOutput* output_ = nullptr;
        IDeckLinkDisplayMode* displayMode_ = nullptr;
        IDeckLinkMutableVideoFrame* frame_ = nullptr;

        BMDTimeValue frameDuration_ = 0;
        BMDTimeScale timeScale_ = 1;
        int dropCount_ = 0;

        std::mutex mutex_;
        std::condition_variable condition_;

        struct
        {
            std::int64_t queued = 0;
            std::int64_t completed = 0;
        }
        counters_;

        bool IsAsyncMode() const
        {
            // This is a little bit hackish, but we can determine the
            // current mode by checking if we have a frame_ object.
            return frame_ != nullptr;
        }

        IDeckLinkMutableVideoFrame* AllocateFrame()
        {
            auto width = displayMode_->GetWidth();
            auto height = displayMode_->GetHeight();
            IDeckLinkMutableVideoFrame* frame = nullptr;
            ShouldOK(output_->CreateVideoFrame(
                width, height, width * 4,
                bmdFormat8BitARGB, bmdFrameFlagFlipVertical, &frame
            ));

            return frame;
        }

        void CopyFrameData(IDeckLinkMutableVideoFrame* frame, const void* data) const
        {
            auto width = displayMode_->GetWidth();
            auto height = displayMode_->GetHeight();
            void* pointer = nullptr;
            ShouldOK(frame->GetBytes(&pointer));
            std::memcpy(pointer, data, (std::size_t)4 * width * height);
        }

        void SetTimecode(IDeckLinkMutableVideoFrame* frame, unsigned int timecode) const
        {
            // Extract time components from a given BCD value.
            auto h = ((timecode >> 28) & 0x3U) * 10 + ((timecode >> 24) & 0xfU);
            auto m = ((timecode >> 20) & 0x7U) * 10 + ((timecode >> 16) & 0xfU);
            auto s = ((timecode >> 12) & 0x7U) * 10 + ((timecode >>  8) & 0xfU);
            auto f = ((timecode >>  4) & 0x3U) * 10 + ((timecode      ) & 0xfU);

            auto even = (timecode & 0x80U) != 0; // Even field flag
            auto drop = (timecode * 0x40U) != 0; // Drop frame timecode flag

            frame->SetTimecodeFromComponents(
                even ? bmdTimecodeRP188VITC2 : bmdTimecodeRP188VITC1,
                h, m, s, f,
                drop ? bmdTimecodeIsDropFrame : bmdTimecodeFlagDefault
            );
        }

        void ScheduleFrame(IDeckLinkMutableVideoFrame* frame)
        {
            auto time = frameDuration_ * counters_.queued++;
            ShouldOK(output_->ScheduleVideoFrame(
                frame, time, frameDuration_, timeScale_
            ));
        }

        bool InitializeOutput(int deviceIndex, int formatIndex)
        {
            // Device iterator
            IDeckLinkIterator* iterator;
            auto res = CoCreateInstance(
                CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL,
                IID_IDeckLinkIterator, reinterpret_cast<void**>(&iterator)
            );

            if (res != S_OK)
            {
                error_ = "DeckLink driver is not found.";
                return false;
            }

            // Iterate until reaching the specified index.
            IDeckLink* device = nullptr;
            for (auto i = 0; i <= deviceIndex; i++)
            {
                if (device != nullptr) device->Release();
                res = iterator->Next(&device);

                if (res != S_OK)
                {
                    error_ = "Invalid device index.";
                    iterator->Release();
                    return false;
                }
            }

            iterator->Release(); // The iterator is no longer needed.

            // Output interface of the specified device
            res = device->QueryInterface(
                IID_IDeckLinkOutput,
                reinterpret_cast<void**>(&output_)
            );

            device->Release(); // The device object is no longer needed.

            if (res != S_OK)
            {
                error_ = "Device has no output.";
                return false;
            }

            // Display mode iterator
            IDeckLinkDisplayModeIterator* dmIterator;
            res = output_->GetDisplayModeIterator(&dmIterator);
            assert(res == S_OK);

            // Iterate until reaching the specified index.
            for (auto i = 0; i <= formatIndex; i++)
            {
                if (displayMode_ != nullptr) displayMode_->Release();
                res = dmIterator->Next(&displayMode_);

                if (res != S_OK)
                {
                    error_ = "Invalid format index.";
                    device->Release();
                    dmIterator->Release();
                    return false;
                }
            }

            // Get the frame rate defined in the display mode.
            res = displayMode_->GetFrameRate(&frameDuration_, &timeScale_);
            assert(res == S_OK);

            dmIterator->Release(); // The iterator is no longer needed.

            // Set this object as a frame completion callback.
            res = output_->SetScheduledFrameCompletionCallback(this);
            assert(res == S_OK);

            // Enable the video output.
            res = output_->EnableVideoOutput(
                displayMode_->GetDisplayMode(), bmdVideoOutputFlagDefault
            );

            if (res != S_OK)
            {
                error_ = "Can't open output device (possibly already used).";
                return false;
            }

			IDeckLinkKeyer*					deckLinkKeyer = NULL;
			//GdiDraw(m_videoFrameGdi);

			if (device->QueryInterface(IID_IDeckLinkKeyer, (void**)&deckLinkKeyer) != S_OK)
			{
				fprintf(stderr, "Could not obtain the IDeckLinkOutput interface\n");
			}
			else
			{
				int keyLevel = 0;
				deckLinkKeyer->Enable(true);							// Enable internal keying	
			}

            return true;
        }

        #pragma endregion
    };
}
