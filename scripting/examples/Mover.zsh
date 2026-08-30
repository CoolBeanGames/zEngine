// Movement behavior: comments must not become executable code.
class Mover : gameObject
{
    /* Engine integration will supply the target GameObject. */
    func move(GameObject obj, Vector3 amount)
    {
        obj.transform.position += amount;
    }
}
