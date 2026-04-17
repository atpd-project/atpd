void service_show_status(atp_config_t *cfg) {
    int pid = service_get_pid(&g_service_ctx);
    
    printf("\n");
    LOG_INFO("==================== ATP Summary ====================");
    
    if (pid > 0) {
        long mem_kb = get_process_memory_kb(pid);
        double cpu = get_process_cpu_percent(pid);
        int threads = get_process_threads(pid);
        int fd_count = get_process_fd_count(pid);
        int uptime_sec = get_process_uptime_sec(pid);
        char uptime_str[64];
        char version[64];
        
        format_uptime(uptime_sec, uptime_str, sizeof(uptime_str));
        get_binary_version(PROXY_BIN_PATH, version, sizeof(version));
        
        LOG_INFO("sing-box is running as root:net_admin. ( PID: %d )", pid);
        LOG_INFO("    ├─ Memory:     %ld kB", mem_kb);
        LOG_INFO("    ├─ CPU:        %.1f%%", cpu);
        LOG_INFO("    ├─ Threads:    %d", threads);
        LOG_INFO("    ├─ Sockets:    %d (Active FDs)", fd_count);
        LOG_INFO("    ├─ Uptime:     %s", uptime_str);
        LOG_INFO("    └─ Version:    %s", version);
    } else {
        LOG_ERROR("sing-box service is stopped.");
    }
    
    LOG_INFO("========================================================");
    printf("\n");
}
