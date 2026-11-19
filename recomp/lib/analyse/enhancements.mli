type flicker_hold = {
  name : string;
  group_class : int;
  anchor_class : int;
  period : int;
  follow : Config.follow;
}

type t = { game_frame_counter : int; flicker_holds : flicker_hold list }

val of_config : Config.t -> Sprite_groups.t -> t
val default : Config.t -> t
val render : t -> Output.t
