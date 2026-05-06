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

    /// <summary>
    /// How long to scan for the ESP32 before giving up and retrying (ms).
    /// </summary>
    private const int ScanTimeoutMs = 15_000;

    /// <summary>
    /// How long to wait before scanning again after a disconnect or error (ms).
    /// </summary>
    private const int ReconnectDelayMs = 5_000;

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
                // Blocks until the ESP32 disconnects or an error occurs
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
        await Task.Delay(1000);

        // 3. Find our service
        // GetPrimaryServiceAsync is unreliable with 128-bit UUIDs in this version
        // of InTheHand — get all services and find ours manually instead
        var services = await device.Gatt.GetPrimaryServicesAsync();
        var service = services.FirstOrDefault(s =>
            s.Uuid == BluetoothUuid.FromGuid(BleConstants.ServiceUuid));

        if (service is null)
            throw new InvalidOperationException("Service UUID not found on device.");

        Status("Service found.");

        // 4. Get the notify characteristic
        var chr = await service.GetCharacteristicAsync(
            BluetoothUuid.FromGuid(BleConstants.CharacteristicUuid));

        if (chr is null)
            throw new InvalidOperationException("Characteristic UUID not found.");

        _characteristic = chr;

        // 5. Subscribe to notifications
        // OnNotification is called for every MTU-sized chunk the ESP32 sends
        chr.CharacteristicValueChanged += OnNotification;
        await chr.StartNotificationsAsync();
        Status("Subscribed — waiting for data...");

        // 6. Block until disconnect or Ctrl+C
        // TaskCompletionSource acts as a gate — execution pauses here until
        // either the ESP32 drops the connection or the cancellation token fires
        var disconnected = new TaskCompletionSource(
            TaskCreationOptions.RunContinuationsAsynchronously);

        device.GattServerDisconnected += (_, _) =>
        {
            Console.WriteLine("[BLE] GattServerDisconnected event fired");
            disconnected.TrySetResult();
        };

        // TrySetResult is used (not SetResult) because both handlers may fire
        // simultaneously — the second call is safely ignored
        ct.Register(() => disconnected.TrySetResult());

        await disconnected.Task;

        // 7. Clean up
        chr.CharacteristicValueChanged -= OnNotification;
        try { await chr.StopNotificationsAsync(); } catch { /* best effort */ }

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
        var found = new TaskCompletionSource<BluetoothDevice?>(
            TaskCreationOptions.RunContinuationsAsynchronously);

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
            try { await _characteristic.StopNotificationsAsync(); } catch { /* best effort */ }
            _characteristic.CharacteristicValueChanged -= OnNotification;
        }

        _device?.Gatt.Disconnect();
    }
}