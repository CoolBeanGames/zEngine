class InputMover : gameObject
{
    export float speed = 2;
    export float jump_height = 1;

    func start()
    {
        Input.action("jump").just_pressed.connect(jump);
    }

    func jump() { transform.position.y += jump_height; }

    func update(float delta)
    {
        Vector3 movement = Input.get_vector("move");
        transform.position.x += movement.x * speed * delta;
        transform.position.z += movement.y * speed * delta;
    }
}
