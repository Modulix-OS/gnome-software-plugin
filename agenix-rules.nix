let
  quentin = "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIHeDXES7JVBTFfXQTezi08nO8GQpWTiQP/myoLfpTAtD quentin@fw-laptop-quentin";
in
{
  "secrets/attic-token.age".publicKeys = [ quentin ];
}
