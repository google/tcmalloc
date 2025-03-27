#![no_std]
use core;
use tcmalloc_sys_extensions;

pub use tcmalloc_sys_extensions::tcmalloc_AddressRegionFactory as TCMallocRegionFactory;

pub use tcmalloc_sys_extensions::absl_Duration_HiRep as AbslHiRep;
pub use tcmalloc_sys_extensions::absl_Duration as AbslDuration;

pub use tcmalloc_sys_extensions::std_map as StdMap;

pub use tcmalloc_sys_extensions::tcmalloc_Profile as Profile;

pub use tcmalloc_sys_extensions::tcmalloc_MallocExtension_AllocationProfilingToken as AllocationProfilingToken;


fn convert_string_to_opaque_array(data: &str) -> tcmalloc_sys_extensions::absl_string_view {
    tcmalloc_sys_extensions::__BindgenOpaqueArray([data.as_ptr() as u64, data.len() as u64])
}

fn convert_opaque_array_to_string(data: tcmalloc_sys_extensions::std_string) -> &'static str {
    // Attempt to convert to a byte slice
    let byte_slice: &[u8] = unsafe {
        // data contains 3 elements
        // 0 -> max size allocated
        // 1 -> used size
        // 2 -> starting address
        core::slice::from_raw_parts(data.0[2] as *const u8, data.0[1] as usize)
    };

    // Attempt to convert the byte slice to a UTF-8 string
    // unsafe may not be needed, but from_utf8_unchecked is a safety hatch incase a non utf8 character is in the stream
    unsafe { core::str::from_utf8_unchecked(byte_slice) }
}

pub fn get_stats() -> &'static str {
    unsafe { convert_opaque_array_to_string(tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetStats()) }
}

pub fn get_numeric_property(property: &str) -> u8 {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetNumericProperty(convert_string_to_opaque_array(property)) }
}

pub fn mark_thread_idle() {
    unsafe {  tcmalloc_sys_extensions::tcmalloc_MallocExtension_MarkThreadIdle() }
}

pub fn mark_thread_busy() {
    unsafe {  tcmalloc_sys_extensions::tcmalloc_MallocExtension_MarkThreadBusy() }
}

pub fn release_cpu_memory(cpu: i32) -> usize {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_ReleaseCpuMemory(cpu) }
}

pub fn get_region_factory() -> *mut TCMallocRegionFactory {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetRegionFactory() }
}

pub fn release_memory_to_system(num_bytes: usize) {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_ReleaseMemoryToSystem(num_bytes) }
}

pub fn get_memory_limit(limit_kind: i32) -> usize {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetMemoryLimit(limit_kind) }
}

pub fn set_memory_limit(limit: usize, limit_kind: i32) {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_SetMemoryLimit(limit, limit_kind) }
}

pub fn get_profile_sampling_interval() -> i64 {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetProfileSamplingInterval() }
}

pub fn set_profile_sampling_interval(interval: i64) {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_SetProfileSamplingInterval(interval) }
}

pub fn get_guarded_sampling_interval() -> i64 {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetGuardedSamplingInterval() }
}

pub fn set_guarded_sampling_interval(interval: i64) {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_SetGuardedSamplingInterval(interval) }
}

pub fn activate_guarded_sampling() {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_ActivateGuardedSampling() }
}

pub fn per_cpu_caches_active() -> bool {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_PerCpuCachesActive() }
}
pub fn get_max_per_cpu_cache_size() -> i32 {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetMaxPerCpuCacheSize() }
}

pub fn set_max_per_cpu_cache_size(value: i32) {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_SetMaxPerCpuCacheSize(value) }
}

pub fn get_max_total_thread_cache_bytes() -> i64 {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetMaxTotalThreadCacheBytes() }
}

pub fn set_max_total_thread_cache_bytes(value: i64) {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_SetMaxTotalThreadCacheBytes(value) }
}

pub fn get_background_process_actions_enabled() -> bool {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetBackgroundProcessActionsEnabled() }
}

pub fn set_background_process_actions_enabled(value: bool) {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_SetBackgroundProcessActionsEnabled(value) }
}

pub fn get_background_process_sleep_interval() -> AbslDuration {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetBackgroundProcessSleepInterval() }
}

pub fn set_background_process_sleep_interval(value: AbslDuration) {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_SetBackgroundProcessSleepInterval(value) }
}

pub fn get_skip_subrelease_short_interval() -> AbslDuration {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetSkipSubreleaseShortInterval() }
}

pub fn set_skip_subrelease_short_interval(value: AbslDuration) {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_SetSkipSubreleaseShortInterval(value) }
}

pub fn get_skip_subrelease_long_interval() -> AbslDuration {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetSkipSubreleaseLongInterval() }
}

pub fn set_skip_subrelease_long_interval(value: AbslDuration) {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_SetSkipSubreleaseLongInterval(value) }
}

pub fn get_cache_demand_based_release() -> bool {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetCacheDemandBasedRelease() }
}

pub fn set_cache_demand_based_release(value: bool) {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_SetCacheDemandBasedRelease(value) }
}

pub fn get_cache_demand_release_short_interval() -> AbslDuration {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetCacheDemandReleaseShortInterval() }
}

pub fn set_cache_demand_release_short_interval(value: AbslDuration) {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_SetCacheDemandReleaseShortInterval(value) }
}

pub fn get_cache_demand_release_long_interval() -> AbslDuration {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetCacheDemandReleaseLongInterval() }
}

pub fn set_cache_demand_release_long_interval(value: AbslDuration) {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_SetCacheDemandReleaseLongInterval(value) }
}

pub fn get_estimated_allocated_size(size: usize) -> usize {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetEstimatedAllocatedSize(size) }
}

pub fn get_estimated_allocated_size_hot_cold(size: usize, hot_cold: u8) -> usize {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetEstimatedAllocatedSize1(size, hot_cold) }
}

pub fn get_allocated_size(p: *const ::core::ffi::c_void) -> u8 {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetAllocatedSize(p) }
}

pub fn get_ownership(p: *const ::core::ffi::c_void) -> i32 {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetOwnership(p) }
}

pub fn get_properties() -> StdMap {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetProperties() }
}

pub fn snapshot_current(r#type: i32) -> Profile {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_SnapshotCurrent(r#type) }
}

pub fn start_allocation_profiling() -> AllocationProfilingToken {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_StartAllocationProfiling() }
}

pub fn start_lifetime_profiling() -> AllocationProfilingToken {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_StartLifetimeProfiling() }
}

pub fn process_background_actions() {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_ProcessBackgroundActions() }
}

pub fn needs_process_background_actions() -> bool {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_NeedsProcessBackgroundActions() }
}

pub fn get_background_release_rate() -> usize {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_GetBackgroundReleaseRate() }
}

pub fn set_background_release_rate(rate: usize) {
    unsafe { tcmalloc_sys_extensions::tcmalloc_MallocExtension_SetBackgroundReleaseRate(rate) }
}

#[cfg(test)]
mod tests {
    use super::{TCMallocRegionFactory, get_numeric_property, get_region_factory, get_stats};
    use tcmalloc_rs;

    #[global_allocator]
    static GLOBAL: tcmalloc_rs::TCMalloc = tcmalloc_rs::TCMalloc;

    #[test]
    fn test_get_stats() {
        let stats = get_stats();
        assert!(!stats.is_empty());
    }

    #[test]
    fn test_get_numeric_property() {
        let value = get_numeric_property("tcmalloc.current_total_thread_cache_bytes");
        assert_ne!(0, value);
    }

    #[test]
    fn test_get_region_factory_internal_byts_allocated() {
        let _value = get_region_factory();
        assert_ne!(0, unsafe { TCMallocRegionFactory::InternalBytesAllocated() });
    }
}