class InspectorBehavior : gameObject
{
    label("Movement");
    export float speed = 5;
    export Vector3 direction = Vector3(1, 0, 0);

    float elapsed = 0; // Hidden from the Inspector, usable by scripts.

    label("Debug");
    export bool showDebug = false;

    func update(float delta)
    {
        elapsed += delta;
        transform.position += direction * speed * delta;
    }
}
