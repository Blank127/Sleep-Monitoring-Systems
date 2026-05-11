using InTheHand.Bluetooth;

namespace ServerBLE;

/// <summary>
/// Manages the full BLE lifecycle for connecting to the ESP32 Sleep Monitor.
/// </summary>
/// <remarks>
/// Handles scan → connect → subscribe → receive → disconnect in a single loop.
/// Automatically reconnects after a disconnect or error.
///
/// Usage:
/// <code>
/// await using var client = new BleClient();
/// client.OnStatus  += msg  => Console.WriteLine(msg);
/// client.OnRawData += data => parser.Feed(data);
/// await client.RunAsync(cancellationToken);
/// </code>
/// </remarks>
internal sealed class BleClient : IAsyncDisposable
{
    // ── Constants ────────────────────────────────────────────────────────────

    /// <summary>How long to scan for the ESP32 before giving up and retrying (ms).</summary>
    private const int ScanTimeoutMs = 15000;

    /// <summary>How long to wait before scanning again after a disconnect or error (ms).</summary>
    private const int ReconnectDelayMs = 10000;

    // ── Private state ─────────────────────────────────────────────────────────

    /// <summary>The connected BLE device. <see langword="null"/> when not connected.</summary>
    private BluetoothDevice? _device;

    /// <summary>The notify characteristic subscribed to for incoming data.</summary>
    private GattCharacteristic? _characteristic;

    /// <summary>
    /// Set to <see langword="true"/> when <see cref="DisposeAsync"/> is called
    /// to stop the <see cref="RunAsync"/> loop cleanly.
    /// </summary>
    private bool _disposed;

    /// <summary>
    /// Guards against <see cref="OnDisconnected"/> being invoked multiple times
    /// for the same disconnect event. Reset at the start of each session.
    /// </summary>
    private bool _disconnectHandled = false;

    // ── Public events ─────────────────────────────────────────────────────────

    /// <summary>
    /// Fired when raw notification bytes arrive from the ESP32.
    /// Subscribe to this event to feed bytes into a <see cref="PacketParser"/>.
    /// </summary>
    public event Action<byte[]>? OnRawData;

    /// <summary>
    /// Fired when the BLE connection state changes.
    /// Provides a human-readable status message suitable for console output.
    /// </summary>
    public event Action<string>? OnStatus;

    /// <summary>
    /// Fired once when the ESP32 disconnects so the caller can close the active session.
    /// Guarded against multiple fires by <see cref="_disconnectHandled"/>.
    /// </summary>
    public event Func<Task>? OnDisconnected;

    // ── Public API ────────────────────────────────────────────────────────────

    /// <summary>
    /// Runs the BLE scan → connect → receive loop until the token is cancelled.
    /// </summary>
    /// <remarks>
    /// Calls <see cref="RunOneSessionAsync"/> repeatedly. If a session throws,
    /// the error is logged and the loop waits <see cref="ReconnectDelayMs"/>
    /// before retrying. <see cref="OperationCanceledException"/> exits cleanly.
    /// </remarks>
    /// <param name="ct">Cancellation token — cancel to stop the loop (e.g. Ctrl+C).</param>
    public async Task RunAsync(CancellationToken ct)
    {
        while (!ct.IsCancellationRequested && !_disposed)
        {
            try
            {
                await RunOneSessionAsync(ct);
            }
            catch (OperationCanceledException) { break; }
            catch (Exception ex)
            {
                Status($"Error: {ex.GetType().Name}: {ex.Message}");
            }

            if (!ct.IsCancellationRequested)
            {
                Status($"Reconnecting in {ReconnectDelayMs / 1000}s...");
                try
                {
                    await Task.Delay(ReconnectDelayMs, ct);
                }
                catch (OperationCanceledException) { break; }
            }
        }

        Status("Stopped.");
    }

    // ── Private methods ───────────────────────────────────────────────────────

    /// <summary>
    /// Runs one full BLE session: scan → connect → subscribe → receive → disconnect.
    /// </summary>
    /// <remarks>
    /// Blocks at step 6 until either the ESP32 disconnects or the cancellation
    /// token fires. Data arrives asynchronously via <see cref="OnNotification"/>
    /// while this method is waiting.
    /// </remarks>
    /// <param name="ct">Cancellation token used to abort the session.</param>
    /// <exception cref="InvalidOperationException">
    /// Thrown if the service or characteristic UUID is not found on the device.
    /// </exception>
    private async Task RunOneSessionAsync(CancellationToken ct)
    {
        // Reset the disconnect guard for this session
        _disconnectHandled = false;

        // 1. Scan
        Status($"Scanning for '{BleConstants.DeviceName}'...");
        var device = await ScanAsync(ct);

        if (device is null)
        {
            Status("Device not found — will retry.");
            return;
        }

        _device = device;
        Status("Device found — connecting...");

        // 2. Connect
        await device.Gatt.ConnectAsync();
        Status("GATT connected.");

        // Give the ESP32 a moment to register the connection before we request services
        await Task.Delay(2000);

        // 3. Find our service
        // GetPrimaryServiceAsync is unreliable with 128-bit UUIDs in this version
        // of InTheHand — get all services and find ours manually instead
        var services = await device.Gatt.GetPrimaryServicesAsync();
        var service = services.FirstOrDefault(s => s.Uuid == BluetoothUuid.FromGuid(BleConstants.ServiceUuid));

        if (service is null)
            throw new InvalidOperationException("Service UUID not found on device.");

        Status("Service found.");
        await Task.Delay(500); // give ESP32 time before characteristic discovery

        // 4. Get the notify characteristic
        var chr = await service.GetCharacteristicAsync(BluetoothUuid.FromGuid(BleConstants.CharacteristicUuid));

        if (chr is null)
            throw new InvalidOperationException("Characteristic UUID not found.");

        _characteristic = chr;

        // 5. Subscribe to notifications
        // OnNotification is called for every MTU-sized chunk the ESP32 sends
        chr.CharacteristicValueChanged += OnNotification;
        await chr.StartNotificationsAsync();
        Status("Subscribed — waiting for data...");

        var disconnected = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);

        // Data watchdog — if no notification arrives within 60s, assume
        // the connection is stale and force a reconnect
        var watchdog = new CancellationTokenSource(TimeSpan.FromSeconds(60));
        watchdog.Token.Register(() =>
        {
            Console.WriteLine("[BLE] Watchdog timeout — forcing reconnect");
            // Explicitly disconnect so Windows releases the connection
            // before we try to scan and reconnect
            try { device.Gatt.Disconnect(); } catch { }
            disconnected.TrySetResult();
        });

        // Reset the watchdog every time data arrives
        Action<byte[]> resetWatchdog = _ =>
        {
            watchdog.CancelAfter(TimeSpan.FromSeconds(60));
        };
        OnRawData += resetWatchdog;

        device.GattServerDisconnected += async (_, _) =>
        {
            if (_disconnectHandled) return;
            _disconnectHandled = true;

            Console.WriteLine("[BLE] GattServerDisconnected event fired");
            if (OnDisconnected is not null)
                await OnDisconnected.Invoke();
            disconnected.TrySetResult();
        };

        ct.Register(() => disconnected.TrySetResult());

        await disconnected.Task;

        // Clean up watchdog
        OnRawData -= resetWatchdog;
        watchdog.Dispose();

        chr.CharacteristicValueChanged -= OnNotification;
        try { await chr.StopNotificationsAsync(); } catch { }

        Status("Disconnected.");
    }

    /// <summary>
    /// Invoked by the BLE stack for every incoming notification chunk from the ESP32.
    /// Forwards the raw bytes to <see cref="OnRawData"/> subscribers.
    /// </summary>
    /// <remarks>
    /// Kept as a named method (not a lambda) so it can be unsubscribed with
    /// <c>-= OnNotification</c>. A lambda cannot be unsubscribed this way.
    /// </remarks>
    /// <param name="_">Sender — unused.</param>
    /// <param name="e">Event args containing the raw notification bytes.</param>
    private void OnNotification(object? _, GattCharacteristicValueChangedEventArgs e)
    {
        if (e.Value is null) return;
        OnRawData?.Invoke(e.Value);
    }

    /// <summary>
    /// Starts a passive BLE scan and returns the first device advertising as
    /// <see cref="BleConstants.DeviceName"/>.
    /// </summary>
    /// <param name="ct">Cancellation token — cancelling aborts the scan immediately.</param>
    /// <returns>
    /// The discovered <see cref="BluetoothDevice"/>, or <see langword="null"/>
    /// if the scan timed out before finding the device.
    /// </returns>
    private static async Task<BluetoothDevice?> ScanAsync(CancellationToken ct)
    {
        var found = new TaskCompletionSource<BluetoothDevice?>(TaskCreationOptions.RunContinuationsAsynchronously);

        // Link the scan timeout with the app cancellation token so that
        // Ctrl+C aborts the scan immediately without waiting for the timeout
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(ct);
        timeout.CancelAfter(ScanTimeoutMs);

        // Local handler — fires for every BLE advertisement received during the scan
        void OnAdvertisement(object? _, BluetoothAdvertisingEvent e)
        {
            if (e.Name == BleConstants.DeviceName)
                found.TrySetResult(e.Device);
        }

        Bluetooth.AdvertisementReceived += OnAdvertisement;

        try
        {
            await Bluetooth.RequestLEScanAsync(new BluetoothLEScanOptions
            {
                AcceptAllAdvertisements = true
            });

            return await found.Task.WaitAsync(timeout.Token);
        }
        catch (OperationCanceledException)
        {
            // Scan timed out or Ctrl+C — return null so the caller retries
            return null;
        }
        finally
        {
            // Always unsubscribe regardless of outcome to prevent memory leaks
            Bluetooth.AdvertisementReceived -= OnAdvertisement;
        }
    }

    /// <summary>
    /// Convenience wrapper — fires <see cref="OnStatus"/> without repeating
    /// the null-conditional invoke everywhere.
    /// </summary>
    /// <param name="msg">Status message to broadcast.</param>
    private void Status(string msg) => OnStatus?.Invoke(msg);

    /// <summary>
    /// Releases BLE resources — stops notifications and disconnects from the device.
    /// Called automatically by <c>await using</c> in Program.cs.
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