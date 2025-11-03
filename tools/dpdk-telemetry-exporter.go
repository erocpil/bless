// go mod init dpdk-telemetry-exporter
// go mod tidy
// go run .
// go build dpdk-telemetry-exporter.go
// --- imports ---
package main

import (
	// "context"
	"encoding/json"
	"fmt"
	"log"
	"net/http"
	"regexp"
	"strings"
	"sync"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"golang.org/x/sys/unix"
)

const telemetrySocket = "/run/dpdk/rte/dpdk_telemetry.v2"
const scrapeInterval = 5 * time.Second
const httpTimeout = 10 * time.Second

// --------------------------- Telemetry Helper ---------------------------

func dpdkQuery(cmd string, timeout time.Duration) (map[string]any, error) {
	fd, err := unix.Socket(unix.AF_UNIX, unix.SOCK_SEQPACKET, 0)
	if err != nil {
		return nil, err
	}
	defer unix.Close(fd)

	// 设置超时
	deadline := time.Now().Add(timeout)
	tv := unix.NsecToTimeval(deadline.Sub(time.Now()).Nanoseconds())
	unix.SetsockoptTimeval(fd, unix.SOL_SOCKET, unix.SO_RCVTIMEO, &tv)
	unix.SetsockoptTimeval(fd, unix.SOL_SOCKET, unix.SO_SNDTIMEO, &tv)

	addr := &unix.SockaddrUnix{Name: telemetrySocket}
	if err := unix.Connect(fd, addr); err != nil {
		return nil, err
	}

	buf := make([]byte, 64)
	if _, _, err := unix.Recvfrom(fd, buf, 0); err != nil {
		return nil, fmt.Errorf("recv handshake failed: %w", err)
	}

	if _, err := unix.Write(fd, []byte(cmd)); err != nil {
		return nil, err
	}

	resp := make([]byte, 65535)
	n, _, err := unix.Recvfrom(fd, resp, 0)
	if err != nil {
		return nil, err
	}

	var parsed map[string]any
	if err := json.Unmarshal(resp[:n], &parsed); err != nil {
		return nil, fmt.Errorf("json parse fail: %w", err)
	}
	return parsed, nil
}

// --------------------------- Metrics ---------------------------

type DpdkExporter struct {
	heapList []int
	portList []int

	heap   *prometheus.GaugeVec
	stats  *prometheus.GaugeVec
	xstats *prometheus.GaugeVec

	mu            sync.RWMutex
	lastScrape    time.Time
	scrapeError   error
	metricsCache  []prometheus.Metric
	stopChan      chan struct{}
	scrapeTicker  *time.Ticker
}

func NewExporter() *DpdkExporter {
	return &DpdkExporter{
		heap: prometheus.NewGaugeVec(
			prometheus.GaugeOpts{Name: "boson__igw__dpdk_eal_heap_info"},
			[]string{"Heap_id", "Name", "metric"},
		),
		stats: prometheus.NewGaugeVec(
			prometheus.GaugeOpts{Name: "boson__igw__dpdk_ethdev_stats"},
			[]string{"port", "metric"},
		),
		xstats: prometheus.NewGaugeVec(
			prometheus.GaugeOpts{Name: "boson__igw__dpdk_ethdev_xstats"},
			[]string{"port", "queue", "metric"},
		),
		stopChan: make(chan struct{}),
	}
}

func (e *DpdkExporter) initHeaps() {
	out, err := dpdkQuery("/eal/heap_list", 2*time.Second)
	if err != nil {
		log.Fatalf("Cannot list heaps: %v", err)
	}

	arr := out["/eal/heap_list"].([]any)
	for _, v := range arr {
		p := int(v.(float64))
		e.heapList = append(e.heapList, p)
	}
	log.Printf("DPDK heaps: %v\n", e.heapList)
}

func (e *DpdkExporter) initPorts() {
	out, err := dpdkQuery("/ethdev/list", 2*time.Second)
	if err != nil {
		log.Fatalf("Cannot list ports: %v", err)
	}

	arr := out["/ethdev/list"].([]any)
	for _, v := range arr {
		p := int(v.(float64))
		e.portList = append(e.portList, p)
	}
	log.Printf("DPDK ports: %v\n", e.portList)
}

// --- Collectors ---

func (e *DpdkExporter) collectHeap() {
	for _, node := range e.heapList {
		key := fmt.Sprintf("/eal/heap_info,%d", node)
		out, err := dpdkQuery(key, 1*time.Second)
		if err != nil {
			log.Printf("Failed to query heap %d: %v", node, err)
			continue
		}

		d := out["/eal/heap_info"].(map[string]any)
		name := d["Name"].(string)
		id := fmt.Sprintf("%v", d["Heap_id"].(float64))

		e.heap.WithLabelValues(id, name, "Heap_size").Set(d["Heap_size"].(float64))
		e.heap.WithLabelValues(id, name, "Free_size").Set(d["Free_size"].(float64))
		e.heap.WithLabelValues(id, name, "Alloc_size").Set(d["Alloc_size"].(float64))
		e.heap.WithLabelValues(id, name, "Greatest_free_size").Set(d["Greatest_free_size"].(float64))
		e.heap.WithLabelValues(id, name, "Alloc_count").Set(d["Alloc_count"].(float64))
		e.heap.WithLabelValues(id, name, "Free_count").Set(d["Free_count"].(float64))
	}
}

func (e *DpdkExporter) collectPortStats() {
	for _, port := range e.portList {
		key := fmt.Sprintf("/ethdev/stats,%d", port)
		out, err := dpdkQuery(key, 1*time.Second)
		if err != nil {
			log.Printf("Failed to query port stats %d: %v", port, err)
			continue
		}

		s := out["/ethdev/stats"].(map[string]any)
		for k, v := range s {
			if val, ok := v.(float64); ok {
				e.stats.WithLabelValues(fmt.Sprint(port), k).Set(val)
			}
		}
	}
}

func (e *DpdkExporter) collectXstats() {
	re := regexp.MustCompile(`_q(\d+)_`)

	for _, port := range e.portList {
		key := fmt.Sprintf("/ethdev/xstats,%d", port)
		out, err := dpdkQuery(key, 1*time.Second)
		if err != nil {
			log.Printf("Failed to query xstats %d: %v", port, err)
			continue
		}

		m := out["/ethdev/xstats"].(map[string]any)
		for k, v := range m {
			val := v.(float64)
			queue := ""

			if match := re.FindStringSubmatch(k); len(match) == 2 {
				queue = match[1]
				k = strings.Replace(k, "_q"+queue+"_", "_", 1)
			}
			e.xstats.WithLabelValues(fmt.Sprint(port), queue, k).Set(val)
		}
	}
}

// --- Background Scraper ---

func (e *DpdkExporter) startBackgroundScraper() {
	e.scrapeTicker = time.NewTicker(scrapeInterval)

	// 立即执行第一次采集
	e.scrapeMetrics()

	go func() {
		for {
			select {
			case <-e.scrapeTicker.C:
				e.scrapeMetrics()
			case <-e.stopChan:
				log.Println("Stopping background scraper")
				return
			}
		}
	}()

	log.Printf("Background scraper started (interval: %v)", scrapeInterval)
}

func (e *DpdkExporter) scrapeMetrics() {
	start := time.Now()

	// 重置指标
	e.heap.Reset()
	e.stats.Reset()
	e.xstats.Reset()

	// 并发采集
	wg := sync.WaitGroup{}
	do := func(fn func()) {
		defer wg.Done()
		fn()
	}

	wg.Add(3)
	go do(e.collectHeap)
	go do(e.collectPortStats)
	go do(e.collectXstats)
	wg.Wait()

	// 收集到临时通道
	ch := make(chan prometheus.Metric, 1000)
	go func() {
		e.heap.Collect(ch)
		e.stats.Collect(ch)
		e.xstats.Collect(ch)
		close(ch)
	}()

	// 缓存指标
	var metrics []prometheus.Metric
	for m := range ch {
		metrics = append(metrics, m)
	}

	// 更新缓存
	e.mu.Lock()
	e.metricsCache = metrics
	e.lastScrape = time.Now()
	e.scrapeError = nil
	e.mu.Unlock()

	duration := time.Since(start)
	log.Printf("Metrics scraped successfully in %v (collected %d metrics)", duration, len(metrics))
}

func (e *DpdkExporter) Stop() {
	if e.scrapeTicker != nil {
		e.scrapeTicker.Stop()
	}
	close(e.stopChan)
}

// --- Prometheus interface ---

func (e *DpdkExporter) Describe(ch chan<- *prometheus.Desc) {
	e.heap.Describe(ch)
	e.stats.Describe(ch)
	e.xstats.Describe(ch)
}

func (e *DpdkExporter) Collect(ch chan<- prometheus.Metric) {
	e.mu.RLock()
	defer e.mu.RUnlock()

	// 返回缓存的指标
	for _, m := range e.metricsCache {
		ch <- m
	}
}

// --------------------------- Main ---------------------------

func main() {
	exporter := NewExporter()
	exporter.initHeaps()
	exporter.initPorts()

	// 启动后台定时采集
	exporter.startBackgroundScraper()
	defer exporter.Stop()

	reg := prometheus.NewRegistry()
	reg.MustRegister(exporter)

	// 配置带超时的 HTTP 服务器
	mux := http.NewServeMux()
	mux.Handle("/metrics", promhttp.HandlerFor(reg, promhttp.HandlerOpts{}))

	server := &http.Server{
		Addr:         ":13050",
		Handler:      mux,
		ReadTimeout:  httpTimeout,
		WriteTimeout: httpTimeout,
		IdleTimeout:  60 * time.Second,
	}

	log.Println("DPDK Exporter ready on :13050/metrics")
	log.Fatal(server.ListenAndServe())
}
