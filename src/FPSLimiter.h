#pragma once

#include <optional>
#include "waitable.h"
#include "vblankmanager.hpp"
#include "rendervulkan.hpp"

uint64_t get_time_in_nanos();

namespace gamescope
{
    struct FPSLimitScheduleTime_t
    {
        // The expected time for the vblank we want to target.
        // (if any)
        uint64_t ulTargetVBlank = 0;
        // The expected latch time to target.
        // (if any)
        uint64_t ulTargetLatch = 0;

        // This is when we want to wake-up by for a game's buffer
        // to meet that latch time above.
        uint64_t ulScheduledWakeupPoint = 0;
    };

    //
    // FPS Limiter via strategic buffer release
    //
    //                                       redzone 
    //                             delta        |
    //                     <------------------><-->
    // --------------------------------------------------------------
    //                     |                   |  |          |
    // --------------------------------------------------------------
    //                     ^                   ^  ^          ^
    //                  release              done |        vblank
    //                                          latch
    //
    class CFPSLimiter : public ITimerWaitable
    {
    public:
        void MarkFrame( Rc<CVulkanTexture> pTexture, uint64_t ulCPUTimestampNanos, uint64_t ulGPUTimestampNanos, bool bReArmTimer = true )
        {
            uint64_t ulDelta = 0;

            {
                std::scoped_lock lock{ pTexture->m_mutBufferTimestamp };

                uint64_t ulReleaseTime = pTexture->m_BufferTimestamp.ulReleaseTime;
                if ( ulReleaseTime )
                {
                    ulDelta = ulGPUTimestampNanos - ulDelta;
                }

                pTexture->m_oBufferTimestamp =
                {
                    .ulCPUTime = ulCPUTimestampNanos,
                    .ulGPUTime = ulGPUTimestampNanos,
                    .ulReleaseTime = ulReleaseTime,
                };
            }

            HoldBuffer( std::move( pTexture ) );

            if ( bReArmTimer )
            {
                // Force timer re-arm with the new timings.
                ArmNextFrame( false );
            }
        }

        FPSLimitScheduleTime_t CalcNextWakeupTime( bool bPreemptive )
        {
            // Get the next wakeup time from the VBlank timer,
            // mark it as pre-emptive so that we don't update/change any state.
            // We just want to peek.
            VBlankScheduleTime nextVBlankSchedule = GetVBlankTimer().CalcNextWakeupTime( true );

            // The time from the last buffer being released to being "done"
            uint64_t ulDelta = m_ulLastGPUTimestamp - m_ulLastRelease;
            uint64_t ulNextReleaseTime = nextVBlankSchedule.ulScheduledWakeupPoint - ulDelta;

            uint64_t ulNow = get_time_in_nanos();
            // Add a bit of slop to now, so we avoid
            // any scheduling quantumn bubbles occuring here.
            uint64_t ulSloppyNow = ulNow + 500'000ul;

            // We should never hit this case... Maybe we should log.
            if ( ulNextReleaseTime > ulSloppyNow )
                ulNextReleaseTime = ulNow;

            return FPSLimitScheduleTime_t
            {
                .ulTargetVBlank = nextVBlankSchedule.ulTargetVBlank,
                .ulTargetLatch  = nextVBlankSchedule.ulScheduledWakeupPoint,
                .ulScheduledWakeupPoint = ulNextReleaseTime,
            };
        }

        void ArmNextFrame( bool bPreemptive )
        {
            std::unique_lock lock( m_ScheduleMutex );

            // If we're pre-emptively re-arming, don't
            // do anything if we are already armed.
            if ( bPreemptive && m_bArmed )
                return;

            m_bArmed = true;
            m_bArmed.notify_all();

            m_TimerFDSchedule = CalcNextWakeupTime( bPreemptive );
            ITimerWaitable::ArmTimer( m_TimerFDSchedule.ulScheduledWakeupPoint );
        }

        void OnPollIn()
        {
			std::unique_lock lock( m_ScheduleMutex );

			// Disarm the timer if it was armed.
			if ( !m_bArmed.exchange( false ) )
				return;

            // Release images.
            ReleaseOldestBuffer();

            // Arm the next frame pre-emptively.
            ArmNextFrame( true );
        }

        // From wlserver_swapchain_feedback/WSI feedback
        void SetTotalBuffers( uint32_t uTotalBuffers )
        {
            m_uTotalBuffers = uTotalBuffers;

            // Release all the buffers when the swapchain feedback changes.
            // New swapchain feedback -> new buffers and buffer count.
            ReleaseAllBuffers();
        }

        // Hold the CVulkanTexture associated with the buffer as
        // it'll handle releasing it via. the backend fb, etc automatically.
        //
        // Better than us doing anything with wlr_buffer here.
        //
        // We should only hold buffers from the WSI layer, linked to a gamescope_swapchain here.
        // Otherwise commits can come from anywhere, xwayland, etc.
        void HoldBuffer( Rc<CVulkanTexture> pTexture ) 
        {
            std::scoped_lock lock{ m_mutHeldBuffers };

            // TODO: Assert that we aren't holding the buffer?
            m_pHeldBuffers.push_back( std::move( pTexture ) );
            uint32_t uNewTotalAcquiredBuffers = ++m_uAcquiredBuffers;
            if ( uNewTotalAcquiredBuffers > m_uTotalBuffers )
            {
                // warn
            }
        }

        uint32_t ReleaseAllBuffers()
        {
            std::scoped_lock lock{ m_mutHeldBuffers };

            m_pHeldBuffers.clear();
            uint32_t uOldTotalAcquiredBuffers = m_uAcquiredBuffers;
            m_uAcquiredBuffers = 0u;
            return uOldTotalAcquiredBuffers;
        }

        uint32_t ReleaseOldestBuffer()
        {
            std::scoped_lock lock{ m_mutHeldBuffers };

            if ( m_pHeldBuffers.empty() )
                return 0u;

            {
                std::scoped_lock lock{ pTexture->m_mutBufferTimestamp };
                m_pHeldBuffers[0]->m_BufferTimestamp = 
            }
            m_pHeldBuffers.erase( m_pHeldBuffers.begin() );

            uint32_t uOldTotalAcquiredBuffers = m_uAcquiredBuffers--;
            return uOldTotalAcquiredBuffers;
        }
    private:
        std::atomic<bool> m_bArmed = { false };
        std::atomic<bool> m_bRunning = { true };

        std::atomic<uint32_t> m_uTotalBuffers = { 1u };
        std::atomic<uint32_t> m_uAcquiredBuffers = { 1u };

        std::mutex m_mutHeldBuffers;
        std::vector<Rc<CVulkanTexture>> m_pHeldBuffers;

        // Should have 0 contest, but just to be safe.
        // This also covers setting of m_bArmed, etc
        // so we keep in sequence.
        // m_bArmed is atomic so can still be .wait()'ed
        // on/read outside.
        // Does not cover m_ulLastVBlank, this is just atomic.
        std::mutex m_ScheduleMutex;
        FPSLimitScheduleTime_t m_TimerFDSchedule{};
    };
}
