class SignalBehavior : gameObject
{
    signal custom_signal;
    export int received = 0;
    export Vector3 last_position;

    func start()
    {
        custom_signal.connect(on_custom);
        transform.was_moved.connect(on_moved);
        custom_signal.emit(7);
    }

    func on_custom(int amount) { received += amount; }
    func on_moved(Vector3 position) { last_position = position; }
    func update(float delta) { transform.position.x += delta; }
}
