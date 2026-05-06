using InTheHand.Bluetooth;

namespace ServerBLE;

internal sealed class BleClient : IAsyncDisposable
{
    // How long to scan before giving up and retrying
    private const int ScanTimeoutMs = 15_000;
    // How long to wait before scanning again after a disconnect or error
    private const int ReconnectDelayMs = 5_000;

    // The connected BLE device — null when not connected
    private BluetoothDevice? _device;
    // The notify characteristic we subscribe to for data
    private GattCharacteristic? _characteristic;
    // Set to true when DisposeAsync is called to stop the loop
    private bool _disposed;

    // Fired when raw bytes arrive from the ESP32 notification
    public event Action<byte[]>? OnRawData;
    // Fired when the connection state changes (e.g. "GATT connected", "Disconnected")
    public event Action<string>? OnStatus;

    /// <summary>
    /// Main loop — keeps running until Ctrl+C or DisposeAsync.
    /// Calls RunOneSessionAsync repeatedly, catching errors and reconnecting automatically.
    /// </summary>
    public async Task RunAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested && !_disposed)
        {
            try
            {
                // Try to connect and receive data — blocks until disconnect or error
                await RunOneSessionAsync(ct);
            }
            catch (OperationCanceledException) { break; } // Ctrl+C — exit cleanly
            catch (Exception ex)
            {
                // Any other error (e.g. UUID not found, connection dropped) — log and retry
                Status($"Error: {ex.GetType().Name}: {ex.Message}");
            }

            if (!ct.IsCancellationRequested)
            {
                Status($"Reconnecting in {ReconnectDelayMs / 1000}s...");
                await Task.Delay(ReconnectDelayMs, ct);
            }
        }

        Status("Stopped.");
    }

    /// <summary>
    /// One full BLE session: scan → connect → subscribe → receive → disconnect.
    /// Blocks at the "waiting for data" stage until the ESP32 disconnects or Ctrl+C is pressed.
    /// </summary>
    private async Task RunOneSessionAsync(CancellationToken ct)
    {
        // ── 1. Scan ────────────────────────────────────────────────────────────
        Status($"Scanning for '{BleConstants.DeviceName}'...");
        var device = await ScanAsync(ct);

        if (device is null)
        {
            Status("Device not found — will retry.");
            return;
        }

        _device = device;
        Status("Device found — connecting...");

        // ── 2. Connect ─────────────────────────────────────────────────────────
        await device.Gatt.ConnectAsync();
        Status("GATT connected.");

        // Give the ESP32 a moment to register the connection before we request services
        await Task.Delay(1000);

        // ── 3. Find our service ────────────────────────────────────────────────
        var services = await device.Gatt.GetPrimaryServicesAsync();
        var service = services.FirstOrDefault(s => s.Uuid == BluetoothUuid.FromGuid(BleConstants.ServiceUuid));

        if (service is null)
            throw new InvalidOperationException("Service UUID not found on device.");

        Status("Service found.");

        // ── 4. Get the notify characteristic ──────────────────────────────────
        var chr = await service.GetCharacteristicAsync(BluetoothUuid.FromGuid(BleConstants.CharacteristicUuid));

        if (chr is null)
            throw new InvalidOperationException("Characteristic UUID not found.");

        _characteristic = chr;

        // ── 5. Subscribe to notifications ──────────────────────────────────────
        // OnNotification will be called every time the ESP32 sends a chunk
        chr.CharacteristicValueChanged += OnNotification;
        await chr.StartNotificationsAsync();
        Status("Subscribed — waiting for data...");

        // ── 6. Block until disconnect or Ctrl+C ────────────────────────────────
        // TaskCompletionSource acts as a gate — we wait here until something signals it
        var disconnected = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        // Signal the gate if the ESP32 disconnects
        device.GattServerDisconnected += (_, _) =>
        {
            Console.WriteLine("[BLE] GattServerDisconnected event fired");
            disconnected.TrySetResult();
        };

        // Signal the gate if Ctrl+C is pressed
        ct.Register(() => disconnected.TrySetResult());

        // Wait here — data arrives via OnNotification in the background
        await disconnected.Task;

        // ── 7. Clean up ────────────────────────────────────────────────────────
        chr.CharacteristicValueChanged -= OnNotification;
        try { await chr.StopNotificationsAsync(); } catch { }

        Status("Disconnected.");
    }

    /// <summary>
    /// Called by the BLE stack every time the ESP32 sends a notification chunk.
    /// Forwards the raw bytes to whoever is subscribed to OnRawData (the PacketParser).
    /// </summary>
    private void OnNotification(object? _, GattCharacteristicValueChangedEventArgs e)
    {
        if (e.Value is null) return;
        OnRawData?.Invoke(e.Value);
    }

    /// <summary>
    /// Starts a BLE scan and returns the first device named "SleepMonitor".
    /// Returns null if the scan times out before finding the device.
    /// </summary>
    private static async Task<BluetoothDevice?> ScanAsync(CancellationToken ct)
    {
        // This task completes when the device is found
        var found = new TaskCompletionSource<BluetoothDevice?>(TaskCreationOptions.RunContinuationsAsynchronously);

        // Link the scan timeout to the app cancellation token
        // so Ctrl+C also cancels the scan
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeout.CancelAfter(ScanTimeoutMs);

        // Local function — called for every BLE advertisement received during the scan
        void OnAdvertisement(object? _, BluetoothAdvertisingEvent e)
        {
            if (e.Name == BleConstants.DeviceName)
                found.TrySetResult(e.Device); // signal the gate — device found
        }

        Bluetooth.AdvertisementReceived += OnAdvertisement;

        try
        {
            // Start passive BLE scan — triggers OnAdvertisement for every nearby device
            await Bluetooth.RequestLEScanAsync(new BluetoothLEScanOptions
            {
                AcceptAllAdvertisements = true
            });

            // Wait until the device is found or the timeout fires
            return await found.Task.WaitAsync(timeout.Token);
        }
        catch (OperationCanceledException)
        {
            // Timed out or Ctrl+C — return null so the caller can retry
            return null;
        }
        finally
        {
            // Always unsubscribe from advertisement events when done scanning
            Bluetooth.AdvertisementReceived -= OnAdvertisement;
        }
    }

    // Convenience helper so we don't repeat OnStatus?.Invoke() everywhere
    private void Status(string msg) => OnStatus?.Invoke(msg);

    /// <summary>
    /// Cleanup — called automatically by "await using" in Program.cs.
    /// Stops notifications and disconnects from the device.
    /// </summary>
    public async ValueTask DisposeAsync()
    {
        _disposed = true;
        if (_characteristic is not null)
        {
            try { await _characteristic.StopNotificationsAsync(); } catch { }
            _characteristic.CharacteristicValueChanged -= OnNotification;
        }
        _device?.Gatt.Disconnect();
    }
}